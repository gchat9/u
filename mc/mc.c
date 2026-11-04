/* mc - two-panel file manager.
 *
 * The screen follows Midnight Commander 4.8 with its default skin, as drawn
 * on a 16-colour xterm. The configuration it assumes is the one that hides
 * the keybar, menubar, message line and xterm title (the [Layout] section of
 * ~/.config/mc/ini):
 *
 *     keybar_visible=false
 *     menubar_visible=false
 *     message_visible=false
 *     xterm_title=false
 *
 * Keys:
 *     Up/Down          select previous/next file or directory
 *     PgUp/PgDn        one screenful up/down, as mc
 *     Home/End         first/last item
 *     Tab              switch between panels
 *     Ctrl+O           show the shell, or the panels again (see below)
 *     Ctrl+U           swap panels around
 *     Enter            a directory or "..": go into it; an executable file:
 *                      run it, and show the panels again when it exits.
 *                      With something on the command line: run that with
 *                      $SHELL -c instead.
 *     F3, ESC 3        mcview on the highlighted file (or enter a directory)
 *     F8, ESC 8        delete the highlighted file, after asking (not directories)
 *     Ctrl+R           read the active panel's directory again
 *     Alt+C            quick cd: type a directory, Enter changes to it
 *     Left/Right, Ctrl+Left/Right, Alt+Backspace, Backspace, Delete
 *                      edit the command line, and the quick cd box
 *     ESC 0, F10       exit (in the panels)
 *
 * The shell is $SHELL -i on a pseudo-terminal of its own. Ctrl+O starts one in
 * the active panel's directory, and shows it; while it is shown every key goes
 * to it, as typed, except Ctrl+O, which shows the panels again. Ctrl+O is
 * refused (a bell) while the shell runs a command: Ctrl+C first. Leaving ends
 * the shell, and no job it started is kept; the active panel then goes to the
 * directory the shell is in. The next Ctrl+O starts another, where the last
 * prompt was. When the shell exits, mc exits too. In the panels, "cd DIR" on
 * the command line is the same as Alt+C DIR.  A panel is read again when its
 * directory has changed, after a command, a viewer or the shell, see rescan_changed().  Alt+S is quick search: the info
 * line takes the letters typed, and the highlight goes to the first name that
 * begins with them, see qs_handle().  Enter on a media file (mp3, ogg,
 * opus, m4a, mp4, webm, mkv, gif, png, webp, jpg, jpeg) runs the viewer of
 * /etc/mc/mc.ext.ini for it, as mc does: the panels go while it runs, and a
 * viewer that fails leaves its output up until a key is pressed.
 *
 * Anything typed in the panels goes to the command line, shown after the
 * prompt. The prompt is PS1 (\u \h \w \$ and friends, and $NAME), or
 * mc's usual user@host:path$ when PS1 is not set.
 *
 * ESC and a following key count as one key if the key comes within a second.
 *
 * Build with DIR_BOLD=1 (make DIR_BOLD=1, after make clean) to draw
 * directories in bold white; by default they are bright white, not bold.
 *
 * Every frame is repainted in full. The terminal size is re-read when
 * SIGWINCH interrupts the wait for a key, so a resize repaints at once.
 */
#include "../_sys/_main.h"
#include "../_lib/width.h"

#define MAXE   4096          /* entries per directory; extras are dropped */
#define NPOOL  131072        /* bytes of names per panel; extras are dropped */
#define MAXW   1024          /* widest terminal we draw */
#define TZMAX  8192          /* room for /etc/localtime */
#define EINTR  4             /* interrupted system call */
#define SIGINT 2
#define SIGQUIT 3
#define SIGCHLD 17
#define POLLIN 1
#define SIGWINCH 28          /* terminal size changed */
#define SA_RESTORER 0x04000000UL
#define AT_FDCWD   -100

/* Kernel termios ABI (generic layout: x86, arm, aarch64, riscv). */
struct ktermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};
struct winsz { uint16_t row, col, xpix, ypix; };

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSCTTY   0x540e
#define TIOCGPTN    0x80045430
#define TIOCGPGRP   0x540f
#define TIOCSPTLCK  0x40045431
#define OPOST       0x00000001
#define IXON        0x00000400
#define ICRNL       0x00000100
#define ISIG        0x00000001
#define ICANON      0x00000002
#define ECHO        0x00000008
#define IEXTEN      0x00008000
#define VTIME       5
#define VMIN        6

#define S_IFMT      0170000
#define S_IFDIR     0040000
#define S_IFREG     0100000
#define S_IFLNK     0120000
#define AT_SYMLINK_NOFOLLOW 0x100

/* Cell attributes: bits 0-2 fg, 3-5 bg, 6 bold, 7 DEC line drawing,
 * 8 terminal default colours. */
#define ATTR(fg, bg, b)  ((fg) | ((bg) << 3) | ((b) << 6))
#define DEC   0x080
#define DEFA  0x100
#define BRIGHT 0x200         /* bright foreground colour (SGR 9x) */
#define BLACK   0
#define RED     1
#define GREEN   2
#define BROWN   3            /* "brown" in mc's skin: plain yellow */
#define BLUE    4
#define MAGENTA 5
#define CYAN    6
#define WHITE   7            /* "lightgray" in mc's skin: plain white */

#define C_FRAME  ATTR(WHITE, BLUE, 0)
#define C_TITLE  ATTR(BLACK, WHITE, 0)
#define C_HEAD   ATTR(BROWN, BLUE, 1)
#define C_CURSOR ATTR(BLACK, CYAN, 0)
#define C_INFO   ATTR(WHITE, BLUE, 0)
#define C_FREE   ATTR(WHITE, BLUE, 0)

/* Directories: bold white with -DDIR_BOLD, otherwise bright white, not bold. */
#ifdef DIR_BOLD
#define C_DIR    ATTR(WHITE, BLUE, 1)
#else
#define C_DIR    (ATTR(WHITE, BLUE, 0) | BRIGHT)
#endif

/* Dialogs, as mc draws them: the quick cd input and the red error box. */
#define C_QCD     ATTR(BLACK, WHITE, 0)
#define C_HIST    ATTR(WHITE, BLUE, 0)    /* the [^] history button */
#define C_SHADE   ATTR(BLACK, BLACK, 1)   /* dialog shadow, keeps the character */
#define C_QTITLE  ATTR(BLUE, WHITE, 0)
#define C_FIELD   ATTR(BLACK, CYAN, 0)
#define C_ERR     ATTR(WHITE, RED, 1)
#define C_ERRHEAD ATTR(BROWN, RED, 1)

/* File classes, in the order mc's filehighlight.ini tests them. */
enum { CL_NORMAL, CL_DIR, CL_EXE, CL_CORE, CL_TEMP, CL_ARCHIVE, CL_DOC,
       CL_SOURCE, CL_MEDIA, CL_GRAPH, CL_DATABASE, CL_STALE };

/* Colours from the [filehighlight] section of mc's default skin. */
static const uint16_t cl_attr[] = {
    ATTR(WHITE,   BLUE, 0),   /* normal: lightgray   */
    C_DIR,                    /* directory           */
    ATTR(GREEN,   BLUE, 1),   /* executable: brightgreen */
    ATTR(RED,     BLUE, 0),   /* core: red           */
    ATTR(BLACK,   BLUE, 1),   /* temp: gray          */
    ATTR(MAGENTA, BLUE, 1),   /* archive: brightmagenta */
    ATTR(BROWN,   BLUE, 0),   /* doc: brown          */
    ATTR(CYAN,    BLUE, 0),   /* source: cyan        */
    ATTR(GREEN,   BLUE, 0),   /* media: green        */
    ATTR(CYAN,    BLUE, 1),   /* graph: brightcyan   */
    ATTR(RED,     BLUE, 1),   /* database: brightred */
    ATTR(RED,     BLUE, 1),   /* stalelink: brightred */
};

/* Extension lists, copied from mc's filehighlight.ini. */
static const struct { const char *ext; uint8_t cls; } cl_ext[] = {
    { "7z;Z;ace;apk;arc;arj;ark;bz2;cab;cpio;deb;gz;lha;lz;lz4;lzh;lzma;rar;rpm;"
      "tar;tbz;tbz2;tgz;tlz;txz;tzst;vsix;xz;zip;zoo;zst", CL_ARCHIVE },
    { "chm;css;ctl;diz;doc;docm;docx;dtd;fodg;fodp;fods;fodt;htm;html;json;letter;"
      "lsm;mail;man;markdown;md;me;mkd;msg;nroff;odg;odp;ods;odt;pdf;po;ppt;pptm;"
      "pptx;ps;rtf;sgml;shtml;tex;text;txt;xls;xlsm;xlsx;xml;xsd;xslt", CL_DOC },
    { "ada;asm;awk;bash;c;c++;caml;cc;cgi;cpp;cxx;diff;erl;go;h;h++;hh;hi;hpp;hs;"
      "inc;jasm;jav;java;js;m4;mak;mjs;ml;mli;mll;mlp;mly;pas;patch;php;phps;pl;pm;"
      "prg;py;rb;s;sas;sh;sl;st;swift;tcl;tk;tsx;xq", CL_SOURCE },
    { "3gp;aac;ac3;ape;asf;avi;dts;flac;flv;it;m3u;m4a;m4v;med;mid;midi;mkv;mod;"
      "mol;mov;mp2;mp3;mp4;mpeg;mpg;mpl;ogg;ogv;opus;s3m;ts;umx;vob;wav;webm;wma;"
      "wmv;xm", CL_MEDIA },
    { "ai;avif;bmp;cdr;eps;gif;ico;jp2;jpeg;jpg;jxl;heic;heif;omf;pcx;pic;png;psb;"
      "psd;rle;svg;tif;tiff;webp;wmf;xbm;xcf;xpm", CL_GRAPH },
    { "cdx;dat;db;dbf;dbi;dbx;fox;mdb;mdn;mdx;msql;mssql;pgsql;sql;ssql", CL_DATABASE },
};

/* A size, in 32 bits: a 30-bit significand and a 2-bit unit (0 bytes, 1 KiB,
 * 1 MiB, 1 GiB).  Exact below 1 GiB; above, to the unit, saturating at 1 EiB.
 * It is for display: a file operation must stat the file again. */
static uint32_t size_pack(uint64_t s)
{
    unsigned u = 0;

    while (u < 3 && s >= (1ull << (30 + 10 * u))) u++;
    if (u) s = (s + (1ull << (10 * u - 1))) >> (10 * u);
    if (s >= (1ull << 30)) s = (1ull << 30) - 1;
    return (uint32_t)((s << 2) | u);
}

static uint64_t size_unpack(uint32_t v)
{
    return (uint64_t)(v >> 2) << (10 * (v & 3));
}

/* One directory entry, 12 bytes.  The name is at p->nm + name (see add_name).
 * The class tells a directory (CL_DIR, which includes a link to one, and ".."),
 * an executable (CL_EXE) and a dangling link (CL_STALE).  The mtime is seconds
 * since 1970 in an unsigned 32-bit number: times before 1970 and after 2105 are
 * not supported. */
struct ent {
    uint32_t name : 24;          /* offset of the name in the pool */
    uint32_t cls  : 4;           /* CL_*: what the entry is, see above */
    uint32_t link : 1;           /* a symbolic link */
    uint32_t up   : 1;           /* the ".." entry */
    uint32_t size;               /* size_pack() */
    uint32_t mtime;              /* seconds since 1970, see above */
};

static int is_dir(const struct ent *e) { return e->cls == CL_DIR; }
static int is_exe(const struct ent *e) { return e->cls == CL_EXE; }
static uint64_t ent_size(const struct ent *e) { return size_unpack(e->size); }
static int64_t ent_mtime(const struct ent *e) { return (int64_t)e->mtime; }

/* The mark before a name: / directory, * executable, ~ link to a directory,
 * @ other link, ! dangling link, or a space. */
static char ent_mark(const struct ent *e)
{
    if (e->link) return e->cls == CL_STALE ? '!' : e->cls == CL_DIR ? '~' : '@';
    return e->cls == CL_DIR ? '/' : e->cls == CL_EXE ? '*' : ' ';
}

struct panel {
    char     path[4096];
    uint64_t dir_mtime, dir_ctime;  /* the directory's, in ns, when it was last read */
    int      n, cur, top;        /* entries shown, cursor, first visible entry */
    int      ne;                 /* entries read, including e[0] = ".." */
    uint64_t free_kb, total_kb;  /* filesystem usage in KiB, 0 if unknown */
    struct ent e[MAXE];          /* e[0] is always ".." */
    uint16_t ord[MAXE];          /* display order: ord[0] == 0, rest sorted */
    int      nn;                 /* bytes of nm in use */
    char     nm[NPOOL];          /* names, NUL-terminated, ".." first */
};

/* A screen cell: a codepoint and its attributes.  CELL_TAIL is the right half
 * of a double-width character, which takes no output of its own. */
#define CELL_TAIL 0xFFFFFFFFu
struct cell { uint32_t c; uint16_t a; };

/* Zero-width codepoints, as vt.c has them: joiners, variation selectors and
 * zero-width spaces.  They are not drawn, and take no cells. */
static int zero_width(uint32_t cp)
{
    return cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x200B && cp <= 0x200F);
}

/* The cells a codepoint takes: two if width.h says it is double-width. */
static int cwidth(uint32_t cp)
{
    if (zero_width(cp)) return 0;
    return cjk_wide(cp) || emoji_wide(cp) ? 2 : 1;
}

/* The codepoint of the UTF-8 character at S[*I] (N bytes in all), which *I is
 * moved past.  A byte that does not start a whole character is U+FFFD. */
static uint32_t utf8_get(const char *s, int n, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    int k = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc0 && c < 0xe0 ? 1 : 0;
    uint32_t cp = k == 3 ? c & 0x07 : k == 2 ? c & 0x0f : k == 1 ? c & 0x1f : c;

    if (c < 0x80) {
        (*i)++;
        return c;
    }
    if (k == 0 || *i + k >= n) {
        (*i)++;
        return 0xfffd;
    }
    for (int j = 1; j <= k; j++) {
        unsigned char d = (unsigned char)s[*i + j];

        if ((d & 0xc0) != 0x80) {
            (*i)++;
            return 0xfffd;
        }
        cp = (cp << 6) | (d & 0x3f);
    }
    *i += k + 1;
    return cp;
}

/* The cells the N bytes of UTF-8 text at S take. */
static int tcols(const char *s, int n)
{
    int w = 0;

    for (int i = 0; i < n; ) w += cwidth(utf8_get(s, n, &i));
    return w;
}

/* The bytes of the longest beginning of the N bytes at S that takes at most MAX
 * cells, cut between characters. */
static int cut_cols(const char *s, int n, int max)
{
    int w = 0, i = 0;

    while (i < n) {
        int at = i, c = cwidth(utf8_get(s, n, &i));

        if (w + c > max) return at;
        w += c;
    }
    return n;
}

static struct panel P[2];
static struct cell  row[MAXW];
static char         ob[4096];    /* output buffer */
static int          on;
static int          cols = 80, rows = 24;
static int          act;         /* index in P[] of the active panel */
static int          lft;         /* index in P[] of the panel on the left */
static int          hidden;      /* Ctrl+O: the normal screen is shown, panels are not */
static int          on_alt;      /* we are on the alternate screen (1049) */
static int          sh_fd = -1;  /* the shell's pseudo-terminal, -1 if not started */
static long         sh_pid;      /* the shell's process */
static int          sh_seen;     /* a shell has been shown before */
static int          cmd_hist;    /* a command has been run: the command line shows mc's [^] */
static int          utf8_lines;  /* the locale is UTF-8: draw lines as Unicode box characters */
static char       **envp;
static int64_t      now;
static uint8_t      tzb[TZMAX];
static long         tzn;
static char         host[65], user[64];
static long         uid;
static int          prompt_len;      /* length of the command line prompt */

/* Modal state: a quick cd input box, or the error box shown after a failed cd
 * (any key closes it). */
enum { MODE_NORMAL, MODE_QCD, MODE_ERR, MODE_QSEARCH, MODE_QUERY };
static int          mode;
/* A line being edited: the quick cd text and the command line. POS is the
 * cursor, FIRST the first character shown. */
struct line {
    char buf[256];
    int  len, pos, first;
};
static struct line  qline, cmdl;
static struct ktermios tty_saved, tty_raw;   /* the terminal before and during mc */
static int          tty_ok;
static char         eline[3][256];   /* error box: header, path, error */
static int          elen[3];
static int          ewidth;          /* outer width of the error box */

/* ---------------------------------------------------------------- output */

static void flush(void)
{
    int off = 0;

    while (off < on) {
        long n = write(1, ob + off, (size_t)(on - off));

        if (n == -EINTR) continue;           /* a window change interrupted us */
        if (n <= 0) break;
        off += (int)n;
    }
    on = 0;
}

static void put(const char *s, int n)
{
    while (n--) {
        if (on == (int)sizeof ob) flush();
        ob[on++] = *s++;
    }
}

static void outs(const char *s) { put(s, (int)strlen(s)); }

static void outu(unsigned long v)
{
    char b[24];
    put(b, (int)utoa(v, b));
}

/* ------------------------------------------------------------ small libc */

static int scmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Value of environment variable NAME, or NULL. */
static const char *genv(const char *name)
{
    for (char **e = envp; *e; e++) {
        const char *s = *e;
        size_t i = 0;
        while (name[i] && name[i] == s[i]) i++;
        if (!name[i] && s[i] == '=') return s + i + 1;
    }
    return NULL;
}

/* Copy s to d, return its length. */
static int cat(char *d, const char *s)
{
    int n = 0;
    while (*s) d[n++] = *s++;
    return n;
}

/* Write V right-aligned, zero-free, into W characters of B. */
static void putnum(char *b, unsigned long v, int w)
{
    for (int i = w - 1; i >= 0; i--) {
        b[i] = (v || i == w - 1) ? (char)('0' + v % 10) : ' ';
        v /= 10;
    }
}

/* Write S centred in W characters of B. */
static void center(char *b, const char *s, int w)
{
    int l = (int)strlen(s), pad = (w - l) / 2;
    for (int i = 0; i < w; i++) b[i] = ' ';
    for (int i = 0; i < l && pad + i < w; i++) b[pad + i] = s[i];
}

/* 64-bit unsigned division. 32-bit targets have no libgcc in this freestanding
 * build, so they get a shift-and-subtract loop; elsewhere the compiler's own. */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
#if __SIZEOF_POINTER__ == 4
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
#else
    if (rem) *rem = n % d;
    return n / d;
#endif
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}

/* ------------------------------------------------------------- sizes */

/* Format SIZE in units from J upwards (0 = bytes, 1 = KiB, ...) the way mc's
 * size_trunc_len() does, into at most LEN characters. Returns the length. */
static int fmt_size(char *b, int len, uint64_t size, int j)
{
    static const uint64_t p10[] = { 1, 10, 100, 1000, 10000, 100000,
                                    1000000, 10000000, 100000000 };
    static const char *const sfx[] = { "", "K", "M", "G", "T", "P", "E" };
    int units = j, n = 0;

    for (; j < 7; j++) {
        if (size == 0) {
            if (j == units) { b[0] = '0'; return 1; }
            if (len > 1) b[n++] = '~';
            n += cat(b + n, j > 1 ? sfx[j - 1] : "B");
            return n;
        }
        if (size < p10[len - (j > 0)]) {
            n = (int)utoa((unsigned long)size, b);
            n += cat(b + n, sfx[j]);
            return n;
        }
        size = (size + 512) >> 10;
    }
    b[0] = '?';
    return 1;
}

/* ------------------------------------------------------------- times */

/* UTC offset in seconds at time T, from /etc/localtime (TZif). Uses the
 * 64-bit block when the file has one. POSIX TZ footers are not read, so DST
 * rules after the file's last explicit transition are not applied. */
static int64_t tz_off(int64_t t)
{
    const uint8_t *h = tzb;
    uint64_t tc, ty, k = 0, i;
    int w = 4;

    if (tzn < 44 || h[0] != 'T' || h[1] != 'Z' || h[2] != 'i' || h[3] != 'f')
        return 0;
    if (h[4] >= '2') {
        /* skip the 32-bit block: times, indices, ttinfo, abbrevs, leaps, flags */
        uint64_t v1 = be(h + 32, 4) * 5 + be(h + 36, 4) * 6 + be(h + 40, 4) +
                      be(h + 28, 4) * 8 + be(h + 24, 4) + be(h + 20, 4);
        if ((uint64_t)tzn < v1 + 88) return 0;
        h += 44 + v1;
        w = 8;
    }
    tc = be(h + 32, 4);
    ty = be(h + 36, 4);
    if ((uint64_t)(h - tzb) + 44 + tc * (w + 1) + ty * 6 > (uint64_t)tzn) return 0;

    const uint8_t *tt = h + 44, *ix = tt + tc * w, *ti = ix + tc;
    for (i = 0; i < tc; i++) {
        int64_t when = w == 8 ? (int64_t)be(tt + i * 8, 8)
                              : (int64_t)(int32_t)(uint32_t)be(tt + i * 4, 4);
        if (when > t) break;
        k = ix[i];
    }
    if (k >= ty) return 0;
    return (int32_t)(uint32_t)be(ti + k * 6, 4);
}

/* Days since 1970-01-01 to a civil date (proleptic Gregorian). 32-bit is
 * enough for any date a file can have, and is cheaper on 32-bit targets. */
static void civil(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;

    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)((int32_t)yoe + era * 400 + (*m <= 2));
}

/* mc's date column, 12 characters: "Nov  1 16:48" when recent,
 * "Nov  1  2025" when older than six months or in the future. */
static void fmt_date(char *b, int64_t mt)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int64_t t = mt + tz_off(mt);
    uint64_t mag = t < 0 ? (uint64_t)-t : (uint64_t)t, rem, q = udiv64(mag, 86400, &rem);
    int64_t days;
    int sod, y, m, d;

    if (t < 0) {                 /* floor division for times before 1970 */
        if (rem) { q++; rem = 86400 - rem; }
        days = -(int64_t)q;
    } else {
        days = (int64_t)q;
    }
    sod = (int)rem;
    civil((int32_t)days, &y, &m, &d);

    b[0] = mon[(m - 1) * 3];
    b[1] = mon[(m - 1) * 3 + 1];
    b[2] = mon[(m - 1) * 3 + 2];
    b[3] = ' ';
    putnum(b + 4, (unsigned long)d, 2);
    if (now > mt + 6L * 30 * 24 * 3600 || now < mt - 3600) {
        b[6] = b[7] = ' ';
        putnum(b + 8, (unsigned long)y, 4);
    } else {
        b[6] = ' ';
        b[7] = (char)('0' + sod / 3600 / 10);
        b[8] = (char)('0' + sod / 3600 % 10);
        b[9] = ':';
        b[10] = (char)('0' + sod % 3600 / 600);
        b[11] = (char)('0' + sod % 3600 / 60 % 10);
    }
}

/* ------------------------------------------------------------ panels */

/* Path as shown in mc: $HOME becomes ~. Returns the length written. */
static int disp_path(char *out, int max, const char *path)
{
    const char *home = genv("HOME");
    size_t hl = home ? strlen(home) : 0;
    int n = 0;

    if (hl > 1 && strlen(path) >= hl) {
        size_t i = 0;
        while (i < hl && home[i] == path[i]) i++;
        if (i == hl && (path[hl] == 0 || path[hl] == '/')) {
            out[n++] = '~';
            path += hl;
        }
    }
    while (*path && n < max - 1) out[n++] = *path++;
    out[n] = 0;
    return n;
}

/* Names compare by the part before the first dot, then the part after it,
 * bytewise. Equivalent to sorting the first dot below every other character,
 * which is what mc's filename collation does (measured against mc 4.8). */
static int ncmp(const char *a, const char *b)
{
    const char *da = a, *db = b;
    size_t la, lb, m;

    while (*da && *da != '.') da++;
    while (*db && *db != '.') db++;
    la = (size_t)(da - a);
    lb = (size_t)(db - b);
    m = la < lb ? la : lb;
    for (size_t i = 0; i < m; i++)
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    if (la != lb) return la < lb ? -1 : 1;
    if (!*da && !*db) return 0;
    if (!*da) return -1;
    if (!*db) return 1;
    return scmp(da + 1, db + 1);
}

/* Directories first, then by name. */
static int ecmp(const struct panel *p, uint16_t x, uint16_t y)
{
    const struct ent *a = &p->e[x], *b = &p->e[y];
    if (is_dir(a) != is_dir(b)) return is_dir(b) - is_dir(a);
    return ncmp(p->nm + a->name, p->nm + b->name);
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Is EXT (no dot) one of the ';'-separated words in LIST? Case-insensitive. */
static int ext_in(const char *list, const char *ext)
{
    for (const char *s = list; *s; ) {
        const char *t = s;
        size_t n = 0;
        int ok = 1;

        while (t[n] && t[n] != ';') n++;
        if (n == 0 || ext[n] != 0) ok = 0;
        for (size_t i = 0; ok && i < n; i++)
            if (lc(s[i]) != lc(ext[i])) ok = 0;
        if (ok) return 1;
        s = t + n;
        if (*s == ';') s++;
    }
    return 0;
}

/* mc's file class for a non-directory: executable, core dump, temp file,
 * then the extension groups, in filehighlight.ini order. */
static uint8_t classify(const char *name, int exe)
{
    const char *dot = NULL, *ext;
    size_t len = strlen(name);

    if (exe) return CL_EXE;
    for (const char *s = name; *s; s++)
        if (*s == '.') dot = s;
    ext = dot ? dot + 1 : "";

    if (name[0] == 'c' && name[1] == 'o' && name[2] == 'r' && name[3] == 'e') {
        const char *r = name + 4;                 /* core, core.1234 */
        while (*r == '.' || (*r >= '0' && *r <= '9')) r++;
        if (!*r) return CL_CORE;
    }
    if (name[0] == '#' || (len && name[len - 1] == '~') ||
        ext_in("bak;part;tmp;$$$", ext))
        return CL_TEMP;
    for (size_t i = 0; i < sizeof cl_ext / sizeof cl_ext[0]; i++)
        if (ext_in(cl_ext[i].ext, ext)) return cl_ext[i].cls;
    return CL_NORMAL;
}

static void canon(char *out, int max, const char *s);

/* Read directory p->path into p->e and sort it. */
/* Append NAME to the name pool of P: its offset, or -1 if the pool is full. */
static int add_name(struct panel *p, const char *name)
{
    int len = (int)strlen(name) + 1, off = p->nn;

    if (off + len > NPOOL) return -1;
    for (int i = 0; i < len; i++) p->nm[off + i] = name[i];
    p->nn += len;
    return off;
}

static int dir_stamp(const char *path, uint64_t *mt, uint64_t *ct)
{
    struct statx sx;

    if (statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &sx) != 0) return -1;
    *mt = (uint64_t)sx.stx_mtime.tv_sec * 1000000000u + sx.stx_mtime.tv_nsec;
    *ct = (uint64_t)sx.stx_ctime.tv_sec * 1000000000u + sx.stx_ctime.tv_nsec;
    return 0;
}

static void load(struct panel *p)
{
    long buf[1024];                 /* 8 KiB, aligned for dirent64 records */
    struct statx sx;
    struct linux_dirent64 *d = NULL;
    struct ent *e = &p->e[0];
    long fd, r, off;

    if (dir_stamp(p->path, &p->dir_mtime, &p->dir_ctime) != 0)
        p->dir_mtime = p->dir_ctime = 0;
    fd = open(p->path, O_RDONLY | O_DIRECTORY, 0);

    /* ".." is always the first entry, and its name is at offset 0 of the pool;
     * its date is the mtime of the parent of the path as mc knows it (lexically,
     * so through a symlink it is not the real parent) */
    p->nn = 0;
    add_name(p, "..");
    e->name = 0;
    e->size = 0;
    e->cls = CL_DIR;
    e->up = 1;
    e->link = 0;
    e->mtime = 0;
    {
        char up[4096], path[4096 + 4];

        strlcpy(path, p->path, sizeof path);
        strlcat(path, "/..", sizeof path);
        canon(up, sizeof up, path);
        if (statx(AT_FDCWD, up, 0, STATX_BASIC_STATS, &sx) == 0)
            e->mtime = (uint32_t)sx.stx_mtime.tv_sec;
    }
    p->ne = 1;

    while (fd >= 0 && (r = getdents64((int)fd, buf, sizeof buf)) > 0) {
        for (off = 0; off < r; off += d->d_reclen) {
            d = (struct linux_dirent64 *)((char *)buf + off);
            const char *nm = d->d_name;
            int at;

            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            if (p->ne >= MAXE) continue;
            at = add_name(p, nm);
            if (at < 0) continue;

            e = &p->e[p->ne];
            e->name = (uint32_t)at;
            e->up = 0;
            e->size = 0;
            e->mtime = 0;
            e->cls = CL_NORMAL;
            e->link = 0;
            if (statx((int)fd, nm, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0) {
                uint32_t type = sx.stx_mode & S_IFMT;
                e->size = size_pack(sx.stx_size);      /* a link's own size and date */
                e->mtime = (uint32_t)sx.stx_mtime.tv_sec;
                if (type == S_IFLNK) {
                    struct statx tx;

                    e->link = 1;
                    if (statx((int)fd, nm, 0, STATX_BASIC_STATS, &tx) != 0)
                        e->cls = CL_STALE;              /* dangling */
                    else if ((tx.stx_mode & S_IFMT) == S_IFDIR)
                        e->cls = CL_DIR;                /* sorts with directories */
                } else if (type == S_IFDIR) {
                    e->cls = CL_DIR;
                } else {
                    e->cls = classify(nm, type == S_IFREG && (sx.stx_mode & 0111));
                }
            }
            p->ne++;
        }
    }
    if (fd >= 0) close((int)fd);

    /* The list: ".." first (not in the root directory), then the rest sorted. */
    {
        int up = !(p->path[0] == '/' && p->path[1] == 0), first = up ? 1 : 0;

        p->n = 0;
        if (up) p->ord[p->n++] = 0;
        for (int i = 1; i < p->ne; i++) p->ord[p->n++] = (uint16_t)i;
        /* insertion sort; plenty fast for ordinary directories */
        for (int i = first + 1; i < p->n; i++) {
            uint16_t v = p->ord[i];
            int j = i;
            while (j > first && ecmp(p, p->ord[j - 1], v) > 0) {
                p->ord[j] = p->ord[j - 1];
                j--;
            }
            p->ord[j] = v;
        }
    }

    {
        struct statfs f;
        p->free_kb = p->total_kb = 0;
        if (statfs(p->path, &f) == 0 && f.f_bsize > 0) {
            p->free_kb  = ((uint64_t)(uid ? f.f_bavail : f.f_bfree) * (uint64_t)f.f_bsize) >> 10;
            p->total_kb = ((uint64_t)f.f_blocks * (uint64_t)f.f_bsize) >> 10;
        }
    }
}

/* Keep the cursor visible. As in mc, crossing the bottom edge moves the list
 * up by half a screen, and crossing the top edge moves it down by half a
 * screen; the top never goes past the last full screen. */
static void scroll(struct panel *p)
{
    int body = rows - 6, half;

    if (body < 1) body = 1;
    half = body / 2 > 0 ? body / 2 : 1;
    if (p->cur >= p->n) p->cur = p->n ? p->n - 1 : 0;
    while (p->cur >= p->top + body) p->top += half;
    if (p->top > p->n - body) p->top = p->n - body > 0 ? p->n - body : 0;
    while (p->cur < p->top && p->top > 0) {
        p->top -= half;
        if (p->top < 0) p->top = 0;
    }
}

/* Read the directory of P again, and keep the highlight on the same name if it
 * is still there, else on the same position. */
static void panel_rescan(struct panel *p)
{
    char name[256];
    int at = p->cur, i;

    name[0] = 0;
    if (p->n > 0) strlcpy(name, p->nm + p->e[p->ord[p->cur]].name, sizeof name);
    load(p);
    p->cur = at < p->n ? at : p->n - 1;
    if (p->cur < 0) p->cur = 0;
    for (i = 0; i < p->n; i++) {
        const char *en = p->nm + p->e[p->ord[i]].name;
        size_t k = 0;

        while (name[k] && name[k] == en[k]) k++;
        if (name[k] == 0 && en[k] == 0) {
            p->cur = i;
            break;
        }
    }
    scroll(p);
}

/* After something may have changed the directories (a command, a viewer, the
 * shell, a deletion), a panel is read again only if its directory has changed:
 * its ctime or mtime differ from when it was read. */
static void rescan_changed(void)
{
    for (int i = 0; i < 2; i++) {
        uint64_t mt = 0, ct = 0;

        if (dir_stamp(P[i].path, &mt, &ct) != 0 || mt != P[i].dir_mtime || ct != P[i].dir_ctime)
            panel_rescan(&P[i]);
    }
}

/* ------------------------------------------------------------ drawing */

static void setc(int x, uint32_t c, uint16_t a)
{
    if (x < 0 || x >= cols) return;
    /* a double-width character whose half is written over is broken: the other
     * half is left blank */
    if (x > 0 && row[x].c == CELL_TAIL) row[x - 1].c = ' ';
    if (x + 1 < cols && row[x + 1].c == CELL_TAIL) row[x + 1].c = ' ';
    row[x].c = c;
    row[x].a = a;
}

static void sets(int x, const char *s, int n, uint16_t a)
{
    for (int i = 0; i < n; ) {
        uint32_t cp = utf8_get(s, n, &i);
        int w = cwidth(cp);

        if (w == 0) continue;
        if (w == 2 && x + 1 >= cols) {          /* a double-width one that does not fit */
            setc(x, ' ', a);
            return;
        }
        setc(x, cp, a);
        if (w == 2) setc(x + 1, CELL_TAIL, a);
        x += w;
    }
}

/* NAME fitted into W characters the way mc's names are: if too long, keep
 * the first W/2 characters and the last W-W/2-1, joined by a ~ in the middle
 * (so the extension survives). Returns the length. */
/* NAME, in W cells: all of it if it fits, else its beginning and its end with a
 * ~ between, as mc truncates names: the beginning as far as W/2 cells, whole
 * characters only, and the end as far as the cells that are left.  Any cells
 * short of W are added at the end.  The bytes go to T; their number is returned. */
static int fit_name(char *t, const char *name, int w)
{
    int l = (int)strlen(name), i = 0, n = 0, cw = 0, total, head = w / 2, at, c, rest, before = 0, j = 0;

    if (w <= 0) return 0;
    total = tcols(name, l);
    if (total <= w) return cat(t, name);
    while (i < l) {
        at = i;
        c = cwidth(utf8_get(name, l, &i));
        if (cw + c > head) {
            i = at;
            break;
        }
        for (int k = at; k < i; k++) t[n++] = name[k];
        cw += c;
    }
    t[n++] = '~';
    cw++;
    rest = w - head - 1;
    while (total - before > rest) before += cwidth(utf8_get(name, l, &j));
    for (int k = j; k < l; k++) t[n++] = name[k];
    cw += total - before;
    while (cw < w) {
        t[n++] = ' ';
        cw++;
    }
    return n;
}

/* Entry marker followed by the fitted name, in at most W characters. */
static int name_text(char *t, int w, const struct panel *p, const struct ent *e)
{
    t[0] = ent_mark(e);
    return 1 + fit_name(t + 1, p->nm + e->name, w - 1);
}

/* One screen row of one panel, drawn into row[] at column X0 with width PW. */
static void panel_row(struct panel *p, int y, int x0, int pw, int active)
{
    int H = rows - 1, nw = pw - 23, body = H - 5;
    int s1 = x0 + 1 + nw, s2 = s1 + 8, r = x0 + pw - 1;
    uint16_t dec = C_FRAME | DEC;
    char t[300], pb[4096];

    if (nw < 1 || H < 6) return;

    if (y == 0) {                                   /* top border with path */
        int tl = disp_path(pb, sizeof pb, p->path), tc;
        tl = cut_cols(pb, tl, pw - 12);
        tc = tcols(pb, tl);
        setc(x0, 'l', dec);
        setc(x0 + 1, '<', C_FRAME);
        setc(x0 + 2, 'q', dec);
        uint16_t tt = active ? C_TITLE : C_FRAME;   /* only the active panel's path is highlighted */
        setc(x0 + 3, ' ', tt);
        sets(x0 + 4, pb, tl, tt);
        setc(x0 + 4 + tc, ' ', tt);
        for (int x = x0 + 5 + tc; x < x0 + pw - 6; x++) setc(x, 'q', dec);
        sets(x0 + pw - 6, ".[^]>", 5, C_FRAME);
        setc(r, 'k', dec);
        return;
    }

    if (y == 1) {                                   /* column headers */
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        t[0] = '.';
        t[1] = 'n';
        center(t + 2, "Name", nw - 2);
        sets(x0 + 1, t, nw, C_HEAD);
        setc(s1, 'x', dec);
        center(t, "Size", 7);
        sets(s1 + 1, t, 7, C_HEAD);
        setc(s2, 'x', dec);
        center(t, "Modify time", 12);
        sets(s2 + 1, t, 12, C_HEAD);
        return;
    }

    if (y >= 2 && y <= body + 1) {                  /* entries */
        int k = p->top + (y - 2);
        int hit = active && k == p->cur && k < p->n;
        uint16_t a = C_FRAME, sep = dec, fill = hit ? C_CURSOR : C_FRAME;

        for (int x = x0 + 1; x < r; x++) setc(x, ' ', fill);
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        if (hit) sep = C_CURSOR | DEC;
        setc(s1, 'x', sep);
        setc(s2, 'x', sep);
        if (k < p->n) {
            const struct ent *e = &p->e[p->ord[k]];
            char sz[8];
            int sl;

            a = hit ? C_CURSOR : cl_attr[e->cls];

            sets(x0 + 1, t, name_text(t, nw, p, e), a);

            if (e->up) sl = cat(sz, "UP--DIR");
            else sl = fmt_size(sz, 7, ent_size(e), 0);
            for (int i = 0; i < 7; i++) t[i] = ' ';
            for (int i = 0; i < sl && i < 7; i++) t[7 - sl + i] = sz[i];
            sets(s1 + 1, t, 7, a);

            fmt_date(t, ent_mtime(e));
            sets(s2 + 1, t, 12, a);
        }
        return;
    }

    if (y == body + 2) {                            /* separator */
        setc(x0, 't', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, 'q', dec);
        setc(r, 'u', dec);
        return;
    }

    if (y == body + 3) {                            /* info line: current entry */
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, ' ', C_INFO);
        if (active && mode == MODE_QSEARCH) {       /* quick search: the letters typed */
            t[0] = '/';
            for (int k = 0; k < qline.len; k++) t[1 + k] = qline.buf[k];
            sets(x0 + 1, t, 1 + qline.len, C_INFO);
            return;
        }
        if (p->n) {
            const struct ent *e = &p->e[p->ord[p->cur]];
            int n;

            if (e->up) {
                n = cat(t, "UP--DIR");
            } else if (e->link) {                   /* "-> target", read now: links are few */
                char tg[256];
                long tl = -1;
                int k = cat(pb, p->path), l;

                if (k && pb[k - 1] != '/') pb[k++] = '/';
                l = cat(pb + k, p->nm + e->name);
                if (k + l < (int)sizeof pb) {
                    pb[k + l] = 0;
                    tl = readlinkat(AT_FDCWD, pb, tg, sizeof tg - 1);
                }
                n = cat(t, "-> ");
                if (tl > 0) {
                    tg[tl] = 0;
                    n += fit_name(t + n, tg, pw - 5);
                } else {
                    n += cat(t + n, "<readlink failed>");
                }
            } else {
                n = name_text(t, pw - 2, p, e);
            }
            sets(x0 + 1, t, n, C_INFO);
        }
        return;
    }

    if (y == body + 4) {                            /* bottom border with free space */
        setc(x0, 'm', dec);
        setc(r, 'j', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, 'q', dec);
        if (p->total_kb) {                          /* " 243G / 252G (96%) " */
            char avail[8], total[8];
            unsigned long pct = (unsigned long)udiv64(100 * p->free_kb, p->total_kb, NULL);
            int la = fmt_size(avail, 5, p->free_kb, 1);
            int lt = fmt_size(total, 5, p->total_kb, 1);
            int n = 0;

            t[n++] = ' ';
            for (int i = 0; i < la; i++) t[n++] = avail[i];
            n += cat(t + n, "/");
            for (int i = 0; i < lt; i++) t[n++] = total[i];
            n += cat(t + n, " (");
            n += (int)utoa(pct, t + n);
            n += cat(t + n, "%) ");
            sets(x0 + pw - 2 - n, t, n, C_FREE);    /* right-aligned before "qj" */
        }
        return;
    }
}

/* Command line: user@host:path$ (or # for root), with the active panel's path. */

/* Put row[0..n) on terminal line Y, emitting SGR and charset changes only
 * where the attributes change. */
/* DEC special graphics, 0x60..0x7e, as Unicode: what the line-drawing cells
 * stand for when the terminal is in a UTF-8 locale. */
static const uint16_t dec_uni[31] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
    0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd,
    0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3,
    0x00b7
};

static uint32_t dec_char(uint32_t c)
{
    return c >= 0x60 && c <= 0x7e ? dec_uni[c - 0x60] : c;
}

static void put_utf8(uint32_t cp)
{
    char b[4];
    int n = 0;

    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xc0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        b[n++] = (char)(0xe0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        b[n++] = (char)(0xf0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    }
    put(b, n);
}

/* Does S name a UTF-8 encoding (UTF-8, utf8, utf-8 ...)? */
static int has_utf8(const char *s)
{
    for (; *s; s++) {
        if ((s[0] | 0x20) == 'u' && (s[1] | 0x20) == 't' && (s[2] | 0x20) == 'f') {
            const char *q = s + 3;

            if (*q == '-') q++;
            if (*q == '8') return 1;
        }
    }
    return 0;
}

static void emit_row(int y, int n)
{
    uint16_t cur = 0xFFFF;
    int dec = 0;

    outs("\x1b[");
    outu((unsigned long)y + 1);
    outs(";1H");
    for (int i = 0; i < n; i++) {
        uint16_t a = row[i].a;
        uint16_t key = a & ~DEC;

        if (row[i].c == CELL_TAIL) continue;    /* the right half of a wide character */
        if (key != cur) {
            cur = key;
            if (a & DEFA) {
                outs("\x1b[0m");
            } else {
                outs("\x1b[0;");
                if (a & 0x40) outs("1;");
                outs((a & BRIGHT) ? "9" : "3");
                outu(a & 7);
                outs(";4");
                outu((a >> 3) & 7);
                outs("m");
            }
        }
        if (((a & DEC) != 0 && !utf8_lines) != dec) {
            dec = !dec;
            outs(dec ? "\x1b(0" : "\x1b(B");
        }
        if ((a & DEC) && utf8_lines) put_utf8(dec_char(row[i].c));
        else if (row[i].c < 0x80) {
            char b = (char)row[i].c;

            put(&b, 1);
        } else put_utf8(row[i].c);
    }
    if (dec) outs("\x1b(B");
    outs("\x1b[0m");
}

/* Copy field FIELD (0 = name, 5 = home directory) of the /etc/passwd entry
 * whose name is NAME (NL bytes), or, when NAME is NULL, whose uid is WANT.
 * Returns 1 if there is such an entry. */
static int pw_field(const char *name, size_t nl, long want, int field, char *out, int max)
{
    static char pw[16384];
    static long pwn = -1;
    long i, e;

    if (pwn < 0) {
        long fd = open("/etc/passwd", O_RDONLY, 0), r;

        pwn = 0;
        if (fd >= 0) {
            while (pwn < (long)sizeof pw && (r = read((int)fd, pw + pwn, sizeof pw - pwn)) > 0)
                pwn += r;
            close((int)fd);
        }
    }
    for (i = 0; i < pwn; i = e + 1) {
        long fs[7], fe[7], k, s0 = i;
        int nf = 0;

        for (e = i; e < pwn && pw[e] != '\n'; e++) ;
        for (k = i; k <= e && nf < 7; k++)              /* split at the colons */
            if (k == e || pw[k] == ':') {
                fs[nf] = s0;
                fe[nf] = k;
                nf++;
                s0 = k + 1;
            }
        if (nf < 6) continue;

        if (name) {
            size_t j = 0;

            if ((size_t)(fe[0] - fs[0]) != nl) continue;
            while (j < nl && pw[fs[0] + (long)j] == name[j]) j++;
            if (j != nl) continue;
        } else {
            unsigned long v = 0;
            int digits = 0;

            for (k = fs[2]; k < fe[2] && pw[k] >= '0' && pw[k] <= '9'; k++) {
                v = v * 10 + (unsigned long)(pw[k] - '0');
                digits++;
            }
            if (!digits || k != fe[2] || (long)v != want) continue;
        }
        for (k = 0; fs[field] + k < fe[field] && k < max - 1; k++)
            out[k] = pw[fs[field] + k];
        out[k] = 0;
        return 1;
    }
    return 0;
}

static void user_name(void)
{
    if (!pw_field(NULL, 0, uid, 0, user, sizeof user))
        user[0] = 0;
}

/* ----------------------------------------------------------- navigation */

static int body_lines(void)
{
    int b = rows - 6;
    return b < 1 ? 1 : b;
}

/* Home and End, as mc: the first item, or the last item with the list scrolled
 * as far as it goes. */
static void go_home(struct panel *p)
{
    p->cur = 0;
    p->top = 0;
}

static void go_end(struct panel *p)
{
    int b = body_lines();

    p->cur = p->n ? p->n - 1 : 0;
    p->top = p->n - b > 0 ? p->n - b : 0;
}

/* PgDn and PgUp, with mc's arithmetic (mc's next_page/prev_page): a step of
 * one screenful of items. */
static void page_down(struct panel *p)
{
    int items = body_lines(), n = p->n;

    if (n == 0 || p->cur == n - 1) return;
    if (p->top > n - 2 * items) items = n - items - p->top;
    if (p->top + items < 0) items = -p->top;
    if (items == 0) p->cur = n - 1;
    else p->cur += items;
    p->top += items;
}

static void page_up(struct panel *p)
{
    int items = body_lines();

    if (p->cur == 0 && p->top == 0) return;
    if (p->top < items) items = p->top;
    if (items == 0) p->cur = 0;
    else p->cur -= items;
    p->top -= items;
}

/* Parse a CSI or SS3 key at I (in[I] is ESC). Sets FIN to the final byte and
 * PAR to the first numeric parameter (-1 if none). Returns the index after the
 * key, or -1 if the key is cut off by the end of the buffer. */

/* Skip one key at I, whatever it is. Returns the index after it. */

/* Up, Down, Home, End, PgUp, PgDn on the active panel. */

static void enter_dir(struct panel *p);
static void zero_block(void *p, size_t n);

/* ----------------------------------------------------------- keys and lines */

#define ESC_WAIT_MS 1000        /* after an ESC, how long the rest of a key may take */
#define K_INCOMPLETE (-1)
#define K_ESC    0x1b
#define K_UP     0x100
#define K_DOWN   0x101
#define K_LEFT   0x102
#define K_RIGHT  0x103
#define K_HOME   0x104
#define K_END    0x105
#define K_PGUP   0x106
#define K_PGDN   0x107
#define K_DEL    0x108
#define K_F3     0x109
#define K_F10    0x10a
#define K_CLEFT  0x10b
#define K_CRIGHT 0x10c
#define K_ALTBS  0x10d
#define K_F8     0x10e
#define K_NONE   0x1ff
#define K_ALT    0x200          /* Alt+x is K_ALT | x */

/* Decode one key at in[i]. Returns the key and sets *len, or K_INCOMPLETE if
 * the buffer ends inside a key. Plain bytes are their own keys. */
static int key_parse(const unsigned char *in, int n, int i, int *len)
{
    int j, k, fin, c, nums[2];

    *len = 1;
    if (in[i] != K_ESC) return in[i];
    if (i + 1 >= n) return K_INCOMPLETE;
    if (in[i + 1] == 0x7f || in[i + 1] == 0x08) { *len = 2; return K_ALTBS; }
    if (in[i + 1] == K_ESC) return K_ESC;       /* ESC ESC: the first is Esc, as in mc */
    if (in[i + 1] != '[' && in[i + 1] != 'O') { *len = 2; return K_ALT | (in[i + 1] & 0x7f); }

    j = i + 2;
    nums[0] = nums[1] = -1;
    k = 0;
    while (j < n && in[j] >= 0x20 && in[j] < 0x40) {   /* parameters and intermediates */
        c = in[j];
        if (c >= '0' && c <= '9') nums[k] = (nums[k] < 0 ? 0 : nums[k]) * 10 + (c - '0');
        else if (c == ';' && k == 0) k = 1;
        j++;
    }
    if (j >= n) return K_INCOMPLETE;
    fin = in[j];
    *len = j + 1 - i;
    switch (fin) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return nums[1] == 5 ? K_CRIGHT : K_RIGHT;
    case 'D': return nums[1] == 5 ? K_CLEFT : K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'R': return in[i + 1] == 'O' ? K_F3 : K_NONE;
    case '~':
        switch (nums[0]) {
        case 1: case 7:  return K_HOME;
        case 4: case 8:  return K_END;
        case 3:          return K_DEL;
        case 5:          return K_PGUP;
        case 6:          return K_PGDN;
        case 13:         return K_F3;
        case 19:         return K_F8;
        case 21:         return K_F10;
        }
        return K_NONE;
    }
    return K_NONE;
}

/* Wait up to MS milliseconds for more input, appending it to BUF after the N
 * bytes already there. Returns the new count. */
static int wait_input(unsigned char *buf, int n, int cap, int ms)
{
    struct pollfd pf;
    long r;

    if (n >= cap) return n;
    pf.fd = 0;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, ms) > 0) {
        r = read(0, buf + n, (size_t)(cap - n));
        if (r > 0) n += (int)r;
    }
    return n;
}

/* Keep the cursor in view in a field W columns wide, of which HIST are taken
 * by a button after it. As mc's input widget: when the cursor leaves the
 * visible part, the text moves so the cursor sits a third of the way in. */
static int char_prev(const struct line *l, int p);

/* The character boundary at most CELLS cells before P in L. */
static int back_cells(const struct line *l, int p, int cells)
{
    int c = 0;

    while (p > 0) {
        int q = char_prev(l, p), i = q;
        int w = cwidth(utf8_get(l->buf, l->len, &i));

        if (c + w > cells) break;
        c += w;
        p = q;
    }
    return p;
}

static void line_view(struct line *l, int w, int hist)
{
    int v = w - hist;

    if (v < 1) v = 1;
    if (l->pos < l->first || tcols(l->buf + l->first, l->pos - l->first) >= v)
        l->first = back_cells(l, l->pos, w / 3);
}

/* The cells from the first character shown to the cursor. */
static int line_cells(const struct line *l)
{
    return l->pos > l->first ? tcols(l->buf + l->first, l->pos - l->first) : 0;
}

static void line_clear(struct line *l)
{
    l->len = l->pos = l->first = 0;
    l->buf[0] = 0;
}

static void line_cut(struct line *l, int a, int b)      /* delete [a, b) */
{
    int k = b - a;

    for (int i = a; i + k <= l->len; i++) l->buf[i] = l->buf[i + k];
    l->len -= k;
    l->pos = a;
    l->buf[l->len] = 0;
}

/* Where the character before P starts, and where the one at P ends. */
static int char_prev(const struct line *l, int p)
{
    if (p > 0) p--;
    while (p > 0 && ((unsigned char)l->buf[p] & 0xc0) == 0x80) p--;
    return p;
}

static int char_next(const struct line *l, int p)
{
    if (p < l->len) p++;
    while (p < l->len && ((unsigned char)l->buf[p] & 0xc0) == 0x80) p++;
    return p;
}

static void line_insert(struct line *l, int c)
{
    for (int i = l->len; i > l->pos; i--) l->buf[i] = l->buf[i - 1];
    l->buf[l->pos++] = (char)c;
    l->len++;
    l->buf[l->len] = 0;
}

/* Where the word before the cursor starts. */
static int word_left(const struct line *l)
{
    int p = l->pos;

    while (p > 0 && l->buf[p - 1] == ' ') p--;
    while (p > 0 && l->buf[p - 1] != ' ') p--;
    return p;
}

/* A key in a line being edited: cursor moves, deletion, typing. Keys that do
 * not edit are ignored. W and HIST give the field for the view. */
static void line_key(struct line *l, int key, int w, int hist)
{
    int p;

    switch (key) {
    case K_LEFT:   l->pos = char_prev(l, l->pos); break;
    case K_RIGHT:  l->pos = char_next(l, l->pos); break;
    case K_HOME:   l->pos = 0; break;
    case K_END:    l->pos = l->len; break;
    case K_CLEFT:  l->pos = word_left(l); break;
    case K_CRIGHT:
        p = l->pos;
        while (p < l->len && l->buf[p] == ' ') p++;
        while (p < l->len && l->buf[p] != ' ') p++;
        l->pos = p;
        break;
    case K_ALTBS:
        p = word_left(l);
        if (p < l->pos) line_cut(l, p, l->pos);
        break;
    case 8:
    case 127:
        if (l->pos > 0) line_cut(l, char_prev(l, l->pos), l->pos);
        break;
    case K_DEL:
        if (l->pos < l->len) line_cut(l, l->pos, char_next(l, l->pos));
        break;
    default:                       /* a byte of a UTF-8 character is typed as it comes */
        if (((key >= 0x20 && key < 0x7f) || (key >= 0x80 && key < 0x100)) &&
            l->len < (int)sizeof l->buf - 1)
            line_insert(l, key);
        break;
    }
    line_view(l, w, hist);
}

/* Draw the visible part of L, V columns wide, at X. */
static void line_draw(const struct line *l, int x, int v, uint16_t a)
{
    int shown = l->len > l->first ? cut_cols(l->buf + l->first, l->len - l->first, v) : 0;

    for (int i = 0; i < v; i++) setc(x + i, ' ', a);
    sets(x, l->buf + l->first, shown, a);
}

/* Bounded append of S to OUT, which holds MAX bytes in all. */
static void addn(char *out, int *n, int max, const char *s)
{
    while (*s && *n < max - 2) out[(*n)++] = *s++;
}

static int idchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Expand PS1 (or mc's default, the usual user@host:path$ prompt) into OUT.
 * Handles \u \h \H \w \W \$ \n \\, drops \[ \] and colour escapes, and
 * expands $NAME and ${NAME} from the environment. */
static int ps1_expand(char *out, int max)
{
    static const char def[] = "\\u@\\h:\\w\\$ ";
    const char *ps = genv("PS1"), *s;
    char tmp[4200];
    int n = 0;

    if (!ps) ps = def;
    for (s = ps; *s && n < max - 2; s++) {
        if (*s == '$' && (s[1] == '{' || idchar(s[1])) && !(s[1] >= '0' && s[1] <= '9')) {
            char name[64];
            const char *b = s + 1, *e, *v;
            int k = 0;

            if (*b == '{') {
                b++;
                for (e = b; *e && *e != '}'; e++) ;
            } else {
                for (e = b; idchar(*e); e++) ;
            }
            while (b < e && k < 63) name[k++] = *b++;
            name[k] = 0;
            v = genv(name);
            addn(out, &n, max, v ? v : "");
            s = (*e == '}' && s[1] == '{') ? e : e - 1;
            continue;
        }
        if (*s != '\\' || !s[1]) {
            out[n++] = *s;
            continue;
        }
        s++;
        switch (*s) {
        case 'u': addn(out, &n, max, user); break;
        case 'h': {
            char h[65];
            int k = 0;

            while (host[k] && host[k] != '.' && k < 64) { h[k] = host[k]; k++; }
            h[k] = 0;
            addn(out, &n, max, h);
            break;
        }
        case 'H': addn(out, &n, max, host); break;
        case 'w': disp_path(tmp, sizeof tmp, P[act].path); addn(out, &n, max, tmp); break;
        case 'W': {
            const char *base = P[act].path;

            for (const char *q = P[act].path; *q; q++)
                if (*q == '/' && q[1]) base = q + 1;
            addn(out, &n, max, base);
            break;
        }
        case '$': out[n++] = uid == 0 ? '#' : '$'; break;
        case 'n': out[n++] = ' '; break;
        case '\\': out[n++] = '\\'; break;
        case '[': case ']': break;
        case 'e':                                  /* skip an ESC [ ... m colour */
            if (s[1] == '[') {
                while (s[1] && s[1] != 'm') s++;
                if (s[1]) s++;
            }
            break;
        default:
            out[n++] = '\\';
            out[n++] = *s;
            break;
        }
    }
    out[n] = 0;
    return n;
}

static int cmd_width(void);

/* The command line: the prompt, then the text typed after it. */
static void prompt_row(void)
{
    char t[4200];
    int n = ps1_expand(t, 4000), w;

    n = cut_cols(t, n, cols / 2);
    prompt_len = tcols(t, n);
    sets(0, t, n, DEFA);
    w = cmd_width();
    line_view(&cmdl, w, 0);
    line_draw(&cmdl, prompt_len, w, DEFA);
    if (cmd_hist && cols >= prompt_len + 8) sets(cols - 3, "[^]", 3, C_HIST);
}

/* Columns for the command line text: after the prompt, and before the [^]
 * (three columns at the right, one blank column before them) once a command
 * has been run. */
static int cmd_width(void)
{
    int w = cols - 1 - prompt_len - (cmd_hist ? 4 : 0);

    return w < 1 ? 1 : w;
}

/* Leave mc's screen for the program that is about to run: back to the normal
 * screen and the terminal as it was. */
static void put_prompt(void);

static void tty_leave(void)
{
    if (tty_ok) ioctl(0, TCSETS, &tty_saved);
    outs("\x1b[0m");
    if (on_alt) {
        outs("\x1b[?1049l");
        on_alt = 0;
    }
    flush();
}

static void tty_enter(void)
{
    if (tty_ok) ioctl(0, TCSETS, &tty_raw);
    if (!hidden && !on_alt) {
        outs("\x1b[?1049h");
        on_alt = 1;
    }
    flush();
}

/* Give the pseudo-terminal FD the size of the terminal. */
static void size_pty(int fd)
{
    struct winsz ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };

    ioctl(fd, TIOCSWINSZ, &ws);
}

/* Through sh, the shell gets PWD as the panel has it (not with symbolic links
 * resolved). $0 is the shell, $1 the directory. Every shell honours PWD. The
 * history of bash (HISTIGNORE) and zsh (HISTORY_IGNORE) is told not to keep
 * the line sh_where() types. */
#define SH_SCRIPT "PWD=$1; export PWD; HISTIGNORE='*PWD}*'; HISTORY_IGNORE='*PWD}*'; export HISTIGNORE HISTORY_IGNORE; exec \"$0\" -i"

/* The shell: $SHELL -i on a pseudo-terminal of its own, made the controlling
 * terminal of a new session, started in the active panel's directory.
 * Returns 0, or -1 if no pseudo-terminal could be had. */
static int sh_start(void)
{
    const char *sh = genv("SHELL");
    char *argv[6], pts[32];
    unsigned int num = 0;
    int zero = 0, n;
    long m, s, pid;

    if (!sh || !*sh) sh = "/bin/sh";
    m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
    if (m < 0) return -1;
    ioctl((int)m, TIOCSPTLCK, &zero);
    if (ioctl((int)m, TIOCGPTN, &num) < 0) {
        close((int)m);
        return -1;
    }
    n = cat(pts, "/dev/pts/");
    n += (int)utoa(num, pts + n);
    pts[n] = 0;
    s = open(pts, O_RDWR | O_NOCTTY, 0);
    if (s < 0) {
        close((int)m);
        return -1;
    }
    size_pty((int)m);
    argv[0] = (char *)"sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)SH_SCRIPT;
    argv[3] = (char *)sh;
    argv[4] = P[act].path;
    argv[5] = NULL;
    pid = clone(SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        setsid();
        ioctl((int)s, TIOCSCTTY, 0);
        dup2((int)s, 0);
        dup2((int)s, 1);
        dup2((int)s, 2);
        close((int)s);
        chdir(P[act].path);
        execve("/bin/sh", argv, envp);
        exit(127);
    }
    close((int)s);
    if (pid < 0) {
        close((int)m);
        return -1;
    }
    sh_fd = (int)m;
    sh_pid = pid;
    return 0;
}

/* Keys typed in the shell view go to the shell as they are. */
static void shell_in(const unsigned char *b, int len)
{
    for (int k = 0; k < len; k++)
        if (b[k] == '\r') cmd_hist = 1;     /* a command has been run: [^] */
    while (len > 0) {
        long w = write(sh_fd, b, (size_t)len);

        if (w == -EINTR) continue;
        if (w <= 0) return;                 /* gone: shell_out() ends mc */
        b += w;
        len -= (int)w;
    }
}

static void change_to(struct panel *p, const char *res, const char *shown);

/* The shell's output goes to the terminal, which shows the normal screen
 * while the shell is shown. When the shell has ended, so does mc, as in mc. */
static void shell_out(void)
{
    long n = read(sh_fd, ob, sizeof ob);

    if (n == -EINTR) return;
    if (n <= 0) {
        tty_leave();
        exit(0);
    }
    on = (int)n;
    flush();
}

/* Wait for input from the terminal. While the shell is shown, its output is
 * copied to the terminal in the meantime. Returns 1 when the terminal has
 * input, 0 when a signal (SIGWINCH) interrupted the wait. */
static int wait_term(void)
{
    struct pollfd pf[2];

    for (;;) {
        long r;

        pf[0].fd = 0;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = sh_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        r = poll(pf, hidden ? 2 : 1, -1);
        if (r == -EINTR) return 0;
        if (r < 0) return 1;                /* read() below ends mc */
        if (hidden && pf[1].revents) shell_out();
        if (pf[0].revents) return 1;
    }
}

/* Ctrl+O leaves the shell only when it is at its prompt: its terminal's
 * foreground process group is the shell itself, not a job it started. */
static int sh_idle(void)
{
    int pg = -1;

    return sh_fd >= 0 && ioctl(sh_fd, TIOCGPGRP, &pg) == 0 && pg == (int)sh_pid;
}

static int same16(const unsigned char *a, const char *b)
{
    int k;

    for (k = 0; k < 16; k++)
        if (a[k] != (unsigned char)b[k]) return 0;
    return 1;
}

/* Ask the idle shell where it is, as the shell itself says: after Ctrl+U clears
 * the line, a line with a leading space (bash keeps such lines out of its
 * history) echoes $PWD between two random tokens of 16 letters, 64 bits. The
 * path is left in ob, NUL-terminated, and its length is returned; -1 if there
 * is no answer within 50 ms. Nothing of the exchange reaches the terminal. */
static long sh_where(void)
{
    unsigned char *b = (unsigned char *)ob;
    char cmd[48], *tok;
    struct pollfd pf;
    long got;
    int k, n = 0, i, j;

    k = cat(cmd, "\x15 echo ");
    tok = cmd + k;                      /* the first token is written where it is sent */
    if (getrandom(tok, 16, 0) != 16) return -1;
    for (i = 0; i < 16; i++) tok[i] = (tok[i] & 15) + 'A';
    k += 16;
    k += cat(cmd + k, "${PWD}");
    for (i = 0; i < 16; i++) cmd[k + i] = tok[i];
    k += 16;
    cmd[k++] = '\r';
    shell_in((const unsigned char *)cmd, k);
    for (;;) {
        pf.fd = sh_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, 50) <= 0) return -1;
        got = read(sh_fd, ob + n, sizeof ob - 1 - (size_t)n);
        if (got <= 0) return -1;
        n += (int)got;
        for (i = 0; i + 17 <= n; i++) {
            if (!same16(b + i, tok) || b[i + 16] != '/') continue;
            for (j = i + 17; j + 16 <= n; j++)
                if (same16(b + j, tok)) {
                    for (k = 0; k < j - i - 16; k++) ob[k] = ob[i + 16 + k];
                    ob[j - i - 16] = 0;
                    return j - i - 16;
                }
            break;                          /* the second token is not in yet */
        }
        if (n >= (int)sizeof ob - 1) return -1;
    }
}

/* Leaving the shell view: the panel goes to the directory the shell is in, if
 * that is not the one it has. Then the shell is ended: closing its
 * pseudo-terminal sends it SIGHUP, so bash saves its history and exits. It is
 * reaped here, and readline's modes are put back. */
static void sh_park(void)
{
    long n;
    int k;

    if (sh_fd < 0) return;
    n = sh_where();
    if (n > 0) {
        for (k = 0; ob[k] && ob[k] == P[act].path[k]; k++) ;
        if (ob[k] || P[act].path[k]) change_to(&P[act], ob, ob);
    }
    close(sh_fd);
    sh_fd = -1;
    wait4((int)sh_pid, NULL, 0, NULL);
    outs("\x1b[?1l\x1b[?2004l");
}

/* Ctrl+O: the shell is shown, or the panels again. Leaving is refused, with a
 * bell, while the shell runs a command. The normal screen is the terminal's own,
 * so the shell's output stays there. A new shell's prompt goes where the last
 * one was: that line is erased from its start first. */
static void toggle_hidden(void)
{
    if (hidden) {
        if (!sh_idle()) {
            outs("\x07");
            flush();
            return;
        }
        hidden = 0;
        sh_park();
        rescan_changed();
        tty_enter();
        return;
    }
    if (sh_fd < 0 && sh_start() < 0) return;
    hidden = 1;
    if (on_alt) {
        outs("\x1b[?1049l");
        on_alt = 0;
    }
    outs(sh_seen ? "\r\x1b[J" : "\r\n");  /* as mc: the first prompt on a new line */
    sh_seen = 1;
    flush();
}

/* Run FILE with ARGV, in directory CWD, and wait for it to exit. With SEARCH,
 * FILE is looked up along $PATH. mc's panels come back afterwards. */
static void put_prompt(void)
{
    char t[4200];
    int n = ps1_expand(t, 4000);

    put(t, n);
}

/* run_child() flags: the file is searched for in PATH; and a child that fails
 * (exits with a non-zero status) leaves its output on the screen until a key is
 * pressed. */
#define RUN_SEARCH 1
#define RUN_PAUSE  2

static void run_child(const char *file, char *const argv[], const char *cwd, int flags, const char *echo)
{
    struct sigaction sa;
    long pid;
    int st = 0;

    tty_leave();
    if (echo) {                           /* a command from the command line, as a shell shows it */
        outs("\r\n");
        put_prompt();
        outs(echo);
        outs("\r\n");
        flush();
    }
    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = SIG_IGN;               /* mc itself does not stop on Ctrl+C */
    rt_sigaction(SIGINT, &sa, NULL, 8);
    rt_sigaction(SIGQUIT, &sa, NULL, 8);

    pid = clone(SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
        zero_block(&sa, sizeof sa);             /* the command gets Ctrl+C; mc's ignore is not inherited */
        sa._u._sa_handler = SIG_DFL;
        rt_sigaction(SIGINT, &sa, NULL, 8);
        rt_sigaction(SIGQUIT, &sa, NULL, 8);
        if (cwd) chdir(cwd);
        if (!(flags & RUN_SEARCH)) {
            execve(file, argv, envp);
        } else {
            const char *path = genv("PATH");
            char buf[4096];

            if (!path) path = "/usr/bin:/bin";
            for (;;) {
                int k = 0;

                while (*path && *path != ':' && k < 4000) buf[k++] = *path++;
                if (k == 0) buf[k++] = '.';
                buf[k] = 0;
                strlcat(buf, "/", sizeof buf);
                strlcat(buf, file, sizeof buf);
                execve(buf, argv, envp);
                if (!*path) break;
                path++;
            }
        }
        exit(127);
    }
    if (pid > 0)
        while (wait4((int)pid, &st, 0, NULL) == -EINTR) ;
    if (echo) {                           /* and the prompt for the next command */
        outs("\r\n");
        put_prompt();
    }

    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = SIG_DFL;
    rt_sigaction(SIGINT, &sa, NULL, 8);
    rt_sigaction(SIGQUIT, &sa, NULL, 8);
    if ((flags & RUN_PAUSE) && (st & 0x7f) == 0 && ((st >> 8) & 0xff) != 0) {
        char key;

        outs("\r\n[failed: press a key] ");
        flush();
        if (tty_ok) ioctl(0, TCSETS, &tty_raw);
        read(0, &key, 1);
    }
    rescan_changed();
    tty_enter();
}

/* Enter on the highlighted item: a directory is entered, an executable file is
 * run. Other files do nothing. */
/* The viewer for a media file, as mc.ext.ini starts it for the same types: the
 * script, in /usr/lib/mc/ext.d, and its file type, in TYPE.  NULL if the file
 * is not a media file.  The extension is matched without regard to case. */
static const char *media_script(const char *name, char *type)
{
    static const char sound[] = "/usr/lib/mc/ext.d/sound.sh";
    static const char video[] = "/usr/lib/mc/ext.d/video.sh";
    static const char image[] = "/usr/lib/mc/ext.d/image.sh";
    const char *dot = NULL, *s;
    size_t n;
    int k;

    for (s = name; *s; s++)
        if (*s == '.') dot = s;
    if (!dot || !dot[1] || (n = strlen(dot + 1)) >= 8) return NULL;
    for (k = 0; k < (int)n; k++) type[k] = (char)lc(dot[1 + k]);
    type[n] = 0;
    if (ext_in("mp3;ogg;opus", type)) return sound;
    if (ext_in("m4a", type)) {
        strlcpy(type, "common", 16);
        return sound;
    }
    if (ext_in("mp4;webm;mkv", type)) {
        strlcpy(type, "ALL_FORMATS", 16);
        return video;
    }
    if (ext_in("gif;png;webp;jpg;jpeg", type)) {
        strlcpy(type, "ALL_FORMATS", 16);
        return image;
    }
    return NULL;
}

/* Enter on a media file: its viewer runs as mc runs a program: the panels go,
 * the viewer has the keys, and the panels come back when it has ended.  The
 * file is in MC_EXT_FILENAME, as the viewers expect. */
static void media_play(struct panel *p, const struct ent *e, const char *script, const char *type)
{
    static const char sh_script[] = "MC_EXT_FILENAME=\"$2\" exec \"$0\" open \"$1\"";
    char file[4352], *argv[7];
    int k;

    k = cat(file, p->path);
    if (k && file[k - 1] != '/') file[k++] = '/';
    k += cat(file + k, p->nm + e->name);
    file[k] = 0;
    argv[0] = (char *)"sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)sh_script;
    argv[3] = (char *)script;
    argv[4] = (char *)type;
    argv[5] = file;
    argv[6] = NULL;
    run_child("/bin/sh", argv, p->path, RUN_PAUSE, NULL);
}

static void enter_item(struct panel *p)
{
    const struct ent *e;
    char *argv[2], dot[300];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (is_dir(e)) {
        enter_dir(p);
        return;
    }
    if (!is_exe(e)) {
        char type[16];
        const char *script = media_script(p->nm + e->name, type);

        if (script) media_play(p, e, script, type);
        return;
    }
    dot[0] = '.';
    dot[1] = '/';
    dot[2] = 0;
    strlcat(dot, p->nm + e->name, sizeof dot);
    argv[0] = dot;
    argv[1] = NULL;
    run_child(dot, argv, p->path, 0, NULL);
}

/* F3: a directory is entered; a file is shown with mcview. */
static void view_item(struct panel *p)
{
    const struct ent *e;
    char *argv[3];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (is_dir(e)) {
        enter_dir(p);
        return;
    }
    argv[0] = (char *)"mcview";
    argv[1] = (char *)(p->nm + e->name);
    argv[2] = NULL;
    run_child("mcview", argv, p->path, RUN_SEARCH, NULL);
}

/* Enter with something on the command line: run it with $SHELL -c. */
static void quick_cd(const char *typed);

static void run_command(void)
{
    char cmd[256], *argv[4];
    const char *sh = genv("SHELL");
    int k;

    if (!sh || !*sh) sh = "/bin/sh";
    for (k = 0; k <= cmdl.len; k++) cmd[k] = cmdl.buf[k];
    line_clear(&cmdl);
    cmd_hist = 1;
    /* "cd DIR" is Alt+C DIR: a child process cannot change the panel's directory */
    if (cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == 0 || cmd[2] == ' ')) {
        quick_cd(cmd + 2);
        return;
    }
    argv[0] = (char *)sh;
    argv[1] = (char *)"-c";
    argv[2] = cmd;
    argv[3] = NULL;
    run_child(sh, argv, P[act].path, 0, cmd);
}

/* Up, Down, Home, End, PgUp and PgDn on the active panel. */
static void nav_key(struct panel *p, int key)
{
    if (key == K_UP) {
        if (p->cur > 0) p->cur--;
    } else if (key == K_DOWN) {
        if (p->cur < p->n - 1) p->cur++;
    } else if (key == K_HOME) {
        go_home(p);
    } else if (key == K_END) {
        go_end(p);
    } else if (key == K_PGUP) {
        page_up(p);
    } else if (key == K_PGDN) {
        page_down(p);
    }
    scroll(p);
}

/* ------------------------------------------------------------- quick cd */

static void change_to(struct panel *p, const char *res, const char *shown);

/* Collapse "." and ".." and repeated slashes in absolute path S into OUT. */
static void canon(char *out, int max, const char *s)
{
    int len = 1;
    const char *q = s;

    out[0] = '/';
    out[1] = 0;
    while (*q) {
        const char *c;
        size_t cl;

        while (*q == '/') q++;
        if (!*q) break;
        c = q;
        while (*q && *q != '/') q++;
        cl = (size_t)(q - c);
        if (cl == 1 && c[0] == '.') continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            out[len] = 0;
            continue;
        }
        if (len > 1 && len < max - 1) out[len++] = '/';
        for (size_t k = 0; k < cl && len < max - 1; k++) out[len++] = c[k];
        out[len] = 0;
    }
}

static const char *errstr(long e)
{
    switch (e) {
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "Input/output error";
    case 13: return "Permission denied";
    case 16: return "Device or resource busy";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 30: return "Read-only file system";
    case 36: return "File name too long";
    case 39: return "Directory not empty";
    case 40: return "Too many levels of symbolic links";
    default: return "Unknown error";
    }
}

/* Open the error box: "Cannot change directory to", the path as typed, and
 * the system error with its number. */
static void set_error(const char *head, const char *shown, long e)
{
    char num[16];
    int w = 0, maxp = cols - 6, l;

    elen[0] = cat(eline[0], head);
    l = cat(eline[1], shown);
    elen[1] = cut_cols(eline[1], l, maxp);
    eline[1][elen[1]] = 0;
    elen[2] = cat(eline[2], errstr(e));
    eline[2][elen[2]++] = ' ';
    eline[2][elen[2]++] = '(';
    l = (int)utoa((unsigned long)e, num);
    for (int i = 0; i < l; i++) eline[2][elen[2]++] = num[i];
    eline[2][elen[2]++] = ')';
    eline[2][elen[2]] = 0;
    for (int i = 0; i < 3; i++) if (tcols(eline[i], elen[i]) > w) w = tcols(eline[i], elen[i]);
    ewidth = w + 6;
    if (ewidth > cols) ewidth = cols;
    mode = MODE_ERR;
}

/* Change the active panel to the directory the user typed. As in mc: blanks
 * around the text are dropped, an empty entry means $HOME, "~" is $HOME, and a
 * relative path is taken from the panel's directory. */
static void quick_cd(const char *typed)
{
    char s[256], shown[512], abs[4096], res[4096];
    struct panel *p = &P[act];
    const char *home = genv("HOME");
    size_t a = 0, b, n = 0;

    b = strlen(typed);
    while (a < b && (typed[a] == ' ' || typed[a] == '\t')) a++;
    while (b > a && (typed[b - 1] == ' ' || typed[b - 1] == '\t' ||
                     typed[b - 1] == '\r' || typed[b - 1] == '\n')) b--;
    for (size_t i = a; i < b; i++) s[n++] = typed[i];
    s[n] = 0;

    if (n == 0) {
        strlcpy(shown, home ? home : "/", sizeof shown);
    } else if (s[0] == '~') {                      /* ~ and ~/ : the account's home */
        size_t k = 1;
        char hb[4096];
        int ok;

        while (s[k] && s[k] != '/') k++;
        ok = k == 1 ? pw_field(NULL, 0, uid, 5, hb, sizeof hb)
                    : pw_field(s + 1, k - 1, 0, 5, hb, sizeof hb);
        if (ok) {
            strlcpy(shown, hb, sizeof shown);
            strlcat(shown, s + k, sizeof shown);
        } else {
            strlcpy(shown, s, sizeof shown);
        }
    } else {
        strlcpy(shown, s, sizeof shown);
    }

    if (shown[0] == '/') {
        strlcpy(abs, shown, sizeof abs);
    } else {
        strlcpy(abs, p->path, sizeof abs);
        if (abs[strlen(abs) - 1] != '/') strlcat(abs, "/", sizeof abs);
        strlcat(abs, shown, sizeof abs);
    }
    canon(res, sizeof res, abs);

    change_to(p, res, shown);
}

/* Change panel P to the absolute directory RES. SHOWN is the path as given,
 * for the error box. Going up selects the directory we came from; anything
 * else starts at the top of the list. */
static void change_to(struct panel *p, const char *res, const char *shown)
{
    char old[4096];
    long fd = open(res, O_RDONLY | O_DIRECTORY, 0);

    if (fd < 0) {
        set_error("Cannot change directory to", shown, -fd);
        return;
    }
    close((int)fd);
    strlcpy(old, p->path, sizeof old);
    strlcpy(p->path, res, sizeof p->path);
    load(p);
    p->cur = 0;
    p->top = 0;
    /* As mc: going up selects the directory we came from, and the window is
     * put around it without being clamped to the end of the list. */
    {
        size_t nl = strlen(res), k, m = 0;
        int up = strlen(old) > nl && (nl == 1 || old[nl] == '/');
        char name[256];

        for (k = 0; up && k < nl; k++)
            if (old[k] != res[k]) up = 0;
        if (up) {
            k = nl == 1 ? 1 : nl + 1;
            while (old[k] && old[k] != '/' && m < sizeof name - 1) name[m++] = old[k++];
            name[m] = 0;
            for (int i = 1; i < p->n; i++) {
                const char *en = p->nm + p->e[p->ord[i]].name;
                size_t j = 0;

                while (name[j] && name[j] == en[j]) j++;
                if (name[j] == 0 && en[j] == 0) {
                    int t = i - (rows - 3) / 2, last = p->n - body_lines();

                    p->cur = i;
                    if (t > last) t = last;             /* the window stops at the last full screen */
                    p->top = t > 0 ? t : 0;
                    break;
                }
            }
        }
    }
}

/* Enter on a directory, or on "..": change the panel to it. Files do nothing. */
static void enter_dir(struct panel *p)
{
    const struct ent *e;
    char abs[4096], res[4096];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (!is_dir(e)) return;
    strlcpy(abs, p->path, sizeof abs);
    if (e->up) {
        strlcat(abs, "/..", sizeof abs);
    } else {
        if (abs[strlen(abs) - 1] != '/') strlcat(abs, "/", sizeof abs);
        strlcat(abs, p->nm + e->name, sizeof abs);
    }
    canon(res, sizeof res, abs);
    change_to(p, res, res);
}

/* Quick cd box: five rows, starting at row rows-7 (one row above the active
 * panel's bottom border), as wide as the panel. Row Y of the box, for the panel
 * at X0 with width PW. The top and bottom rows are blank margins. */
static void qcd_draw_row(int y, int x0, int pw)
{
    int dy = y - (rows - 7), r = x0 + pw - 1, f = pw - 12, x;
    const char *title = " Quick cd ";

    for (x = x0; x <= r; x++) setc(x, ' ', C_QCD);
    if (dy == 0 || dy == 4) return;                /* margins */
    if (dy == 1 || dy == 3) {                      /* frame */
        setc(x0 + 1, dy == 1 ? 'l' : 'm', C_QCD | DEC);
        for (x = x0 + 2; x <= x0 + pw - 3; x++) setc(x, 'q', C_QCD | DEC);
        setc(x0 + pw - 2, dy == 1 ? 'k' : 'j', C_QCD | DEC);
        if (dy == 1) sets(x0 + 2 + (pw - 4 - 10) / 2, title, 10, C_QTITLE);
    } else {                                       /* input line */
        setc(x0 + 1, 'x', C_QCD | DEC);
        setc(x0 + pw - 2, 'x', C_QCD | DEC);
        sets(x0 + 3, "cd", 2, C_QCD);
        line_view(&qline, pw - 9, 3);
        line_draw(&qline, x0 + 6, f, C_FIELD);
        sets(x0 + 6 + f, "[^]", 3, C_HIST);
    }
}

/* ------------------------------------------------------------ error box */

/* Position of the error box: mc's wtools placement for message dialogs. */
static void err_geom(int *x, int *y)
{
    int h = 9, w = ewidth;
    int ypos = rows / 3 - (h - 3) / 2;

    if (ypos + h < rows / 2) {                     /* too high: centre it */
        *y = (rows - h) / 2;
        *x = (cols - w) / 2;
    } else {
        *y = ypos;
        *x = cols / 2 - w / 2;
    }
    if (*y < 0) *y = 0;
    if (*x < 0) *x = 0;
}

static void err_draw_row(int y, int ex, int ey)
{
    int dy = y - ey, w = ewidth, x0 = ex, r = ex + w - 1, x, i;

    if (dy < 0 || dy >= 9) return;
    for (x = x0; x <= r; x++) setc(x, ' ', C_ERR);
    if (dy == 1) {                                 /* top frame with title */
        setc(x0 + 1, 'l', C_ERR | DEC);
        for (x = x0 + 2; x <= x0 + w - 3; x++) setc(x, 'q', C_ERR | DEC);
        setc(x0 + w - 2, 'k', C_ERR | DEC);
        sets(x0 + 2 + (w - 4 - 7) / 2, " Error ", 7, C_ERRHEAD);
    } else if (dy == 7) {                          /* bottom frame */
        setc(x0 + 1, 'm', C_ERR | DEC);
        for (x = x0 + 2; x <= x0 + w - 3; x++) setc(x, 'q', C_ERR | DEC);
        setc(x0 + w - 2, 'j', C_ERR | DEC);
    } else if (dy >= 2 && dy <= 6) {               /* sides */
        setc(x0 + 1, 'x', C_ERR | DEC);
        setc(x0 + w - 2, 'x', C_ERR | DEC);
        if (dy >= 3 && dy <= 5) {                  /* message lines, centred */
            i = dy - 3;
            sets(x0 + 2 + (w - 4 - tcols(eline[i], elen[i])) / 2, eline[i], elen[i], C_ERR);
        }
    }
}

/* Keep the cursor visible in the quick cd field, as mc's input widget does:
 * the field is the panel width less 9 (its 3-column [^] button is inside
 * that), and when the cursor leaves it the text moves so the cursor is a
 * third of the width in from the left. */
static int act_pw(void)
{
    return act == lft ? cols / 2 : cols - cols / 2;
}


/* Keys while a dialog is open. */

/* Keys in the quick cd box. Enter goes to the directory typed; Esc closes. */
static void qcd_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return;
    }
    if (key == '\r' || key == '\n') {
        char t[256];
        int k;

        for (k = 0; k <= qline.len; k++) t[k] = qline.buf[k];
        mode = MODE_NORMAL;
        if (qline.len) quick_cd(t);
        return;
    }
    line_key(&qline, key, act_pw() - 9, 3);
}

/* Shadow cells x0..x1 of row Y: the character stays, the colour goes dark. */
static void shade(int x0, int x1)
{
    for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < cols; x++)
        row[x].a = C_SHADE | (row[x].a & DEC);   /* a line-drawing cell stays one */
}

/* The dialogs of a file operation, as mc shows them: a box in the middle of the
 * screen with a title, two lines of text and buttons, one of them selected.  The
 * question is Yes or No; a failure is Skip, Skip all, Retry or Abort.  Only
 * files are deleted: F8 on a directory does nothing. */
enum { QY_ASK, QY_ERR };
struct qy_button { const char *label; int hot; };   /* hot: the index of the hotkey in the label */
static const struct qy_button qy_yesno[2] = { { "[ Yes ]", 2 }, { "[ No ]", 2 } };
static const struct qy_button qy_failed[4] = { { "[ Skip ]", 2 }, { "[ Skip all ]", 5 },
                                               { "[ Retry ]", 2 }, { "[ Abort ]", 2 } };
static int  qy_kind;
static char qy_t1[320], qy_t2[320];     /* the two lines of text */
static int  qy_n1, qy_n2;
static int  qy_sel;                     /* the selected button */
static int  qrx, qry, qrw, qrh;         /* the box, with its margin of one cell */

static const struct qy_button *qy_buttons(int *n)
{
    *n = qy_kind == QY_ASK ? 2 : 4;
    return qy_kind == QY_ASK ? qy_yesno : qy_failed;
}

/* The box, and where it goes.  Its width is that of the widest of the title, the
 * text and the buttons, each button with two cells more than its label.  Where it
 * goes is as mc puts it: the question is placed as a box of six lines, in the upper
 * third of the screen; a failure is placed as a box of four lines, centred, and
 * then grows downwards to its eight. */
static void qy_geom(void)
{
    int nb, win = 0, m, t1 = tcols(qy_t1, qy_n1), t2 = tcols(qy_t2, qy_n2);
    const struct qy_button *b = qy_buttons(&nb);

    for (int i = 0; i < nb; i++) win += (int)strlen(b[i].label) + 2;
    m = win;
    if (6 > m) m = 6;                   /* the title: "Delete", 5 for "Error" */
    if (t1 > m) m = t1;
    if (t2 > m) m = t2;
    qrw = m + 6;
    qrh = 2 + 6;                        /* two lines of text, and the frame, with the margin */
    qrx = (cols - qrw + 1) / 2;
    qry = qy_kind == QY_ASK ? (rows - 6) / 3 : (rows - 4) / 2;
    if (qrx < 0) qrx = 0;
    if (qry < 0) qry = 0;
}

/* One button's label, in its place: the hotkey in brown. */
static void qy_label(int x, const char *label, int hot, uint16_t base, uint16_t key)
{
    for (int k = 0; label[k]; k++)
        setc(x + k, (unsigned char)label[k], k == hot ? key : base);
}

static void qy_draw_row(int y)
{
    int fx = qrx + 1, fw = qrw - 2, fh = qrh - 2, in = fw - 2, dy = y - (qry + 1), x, nb;
    const struct qy_button *b;

    if (y < qry || y >= qry + qrh) return;
    for (x = qrx; x < qrx + qrw; x++) setc(x, ' ', C_ERR);
    if (dy < 0 || dy >= fh) return;
    setc(fx, dy == 0 ? 'l' : dy == 5 ? 'm' : dy == 3 ? 't' : 'x', C_ERR | DEC);
    setc(fx + fw - 1, dy == 0 ? 'k' : dy == 5 ? 'j' : dy == 3 ? 'u' : 'x', C_ERR | DEC);
    if (dy == 0 || dy == 3 || dy == 5)
        for (x = fx + 1; x < fx + fw - 1; x++) setc(x, 'q', C_ERR | DEC);
    if (dy == 0) {
        const char *title = qy_kind == QY_ASK ? " Delete " : " Error ";
        int tl = (int)strlen(title);

        sets(fx + 1 + (in - tl) / 2, title, tl, C_ERRHEAD);
    }
    if (dy == 1) sets(fx + 1 + (in - tcols(qy_t1, qy_n1)) / 2, qy_t1, qy_n1, C_ERR);
    if (dy == 2) sets(fx + 1 + (in - tcols(qy_t2, qy_n2)) / 2, qy_t2, qy_n2, C_ERR);
    if (dy == 4) {
        int block = 0, at;

        b = qy_buttons(&nb);
        for (int i = 0; i < nb; i++) block += (int)strlen(b[i].label) + (i ? 2 : 0);
        at = fx + 1 + (in - block) / 2;
        for (int i = 0; i < nb; i++) {
            int sel = qy_sel == i;
            uint16_t base = sel ? C_TITLE : C_ERR;
            uint16_t key = sel ? ATTR(BROWN, WHITE, 1) : C_ERRHEAD;

            qy_label(at, b[i].label, b[i].hot, base, key);
            at += (int)strlen(b[i].label) + 2;
        }
    }
}

/* F8 or ESC 8 on a file: the question.  The name is shown as mc shows it, cut to
 * 43 cells in the middle: 64 cells less the question's own 17 and 4 for the box. */
static void delete_open(struct panel *p)
{
    const struct ent *e;
    char fitted[256];
    int n;

    if (p->n == 0) return;
    e = &p->e[p->ord[p->cur]];
    if (e->up || is_dir(e)) return;
    n = fit_name(fitted, p->nm + e->name, 43);
    qy_kind = QY_ASK;
    qy_n1 = cat(qy_t1, "Delete file");
    qy_n2 = 0;
    qy_t2[qy_n2++] = '"';
    for (int k = 0; k < n; k++) qy_t2[qy_n2++] = fitted[k];
    qy_t2[qy_n2++] = '"';
    qy_t2[qy_n2++] = '?';
    qy_t2[qy_n2] = 0;
    qy_sel = 0;
    mode = MODE_QUERY;
}

/* Remove the highlighted file.  If that fails, the box asks what to do: Skip,
 * Skip all, Retry, or Abort.  If it works, the panels read their directories
 * again where those have changed. */
static void delete_attempt(struct panel *p)
{
    const struct ent *e;
    char path[4352];
    int k;
    long r;

    if (p->n == 0) return;
    e = &p->e[p->ord[p->cur]];
    k = cat(path, p->path);
    if (k && path[k - 1] != '/') path[k++] = '/';
    k += cat(path + k, p->nm + e->name);
    path[k] = 0;
    r = unlinkat(AT_FDCWD, path, 0);
    if (r >= 0) {
        mode = MODE_NORMAL;
        rescan_changed();
        return;
    }
    qy_kind = QY_ERR;
    qy_n1 = cat(qy_t1, "Cannot remove file \"");
    k = 0;
    while (path[k] && qy_n1 + k < (int)sizeof qy_t1 - 3) {
        qy_t1[qy_n1 + k] = path[k];
        k++;
    }
    qy_n1 += k;
    qy_t1[qy_n1++] = '"';
    qy_t1[qy_n1] = 0;
    qy_n2 = cat(qy_t2, errstr(-r));
    qy_t2[qy_n2++] = ' ';
    qy_t2[qy_n2++] = '(';
    k = (int)utoa((unsigned long)-r, qy_t2 + qy_n2);
    qy_n2 += k;
    qy_t2[qy_n2++] = ')';
    qy_t2[qy_n2] = 0;
    qy_sel = 0;
    mode = MODE_QUERY;
}

/* The choice of a button: Yes deletes the file; of the failure's buttons, Retry
 * tries again, and the others close the box. */
static void qy_choose(int b)
{
    if (qy_kind == QY_ASK) {
        if (b == 0) delete_attempt(&P[act]);
        else mode = MODE_NORMAL;
    } else if (b == 2) {
        delete_attempt(&P[act]);
    } else {
        mode = MODE_NORMAL;
    }
}

/* The keys of a question: its hotkeys, Enter for the selected button, Left and
 * Right (or Tab) to choose, and Esc, which is No or Abort. */
static int qy_handle(int key)
{
    int nb;
    const struct qy_button *b = qy_buttons(&nb);

    if (key == K_ESC) {
        qy_choose(qy_kind == QY_ASK ? 1 : 3);
        return 0;
    }
    if (key == 13 || key == 10) {
        qy_choose(qy_sel);
        return 0;
    }
    if (key == K_LEFT || key == K_RIGHT || key == 9) {
        qy_sel = key == K_LEFT ? (qy_sel + nb - 1) % nb : (qy_sel + 1) % nb;
        return 0;
    }
    for (int i = 0; i < nb; i++) {
        int h = (unsigned char)b[i].label[b[i].hot], k = key;

        if (h >= 'A' && h <= 'Z') h += 32;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (k == h) {
            qy_choose(i);
            return 0;
        }
    }
    return 0;
}

static void draw(void)
{
    int lw = cols / 2, ax = act == lft ? 0 : lw, aw = act == lft ? lw : cols - lw;
    int ex = 0, ey = 0, cy, cx;

    if (hidden) {                     /* the normal screen is the terminal's own */
        flush();
        return;
    }

    if (mode == MODE_ERR) err_geom(&ex, &ey);
    if (mode == MODE_QUERY) qy_geom();
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            row[x].c = ' ';
            row[x].a = DEFA;
        }
        if (y == rows - 1) {
            prompt_row();
        } else if (!hidden) {
            panel_row(&P[lft], y, 0, lw, act == lft);
            panel_row(&P[lft ^ 1], y, lw, cols - lw, act == (lft ^ 1));
        }
        if (mode == MODE_QCD && y >= rows - 7 && y <= rows - 3)
            qcd_draw_row(y, ax, aw);
        if (mode == MODE_ERR && y >= ey && y <= ey + 8)
            err_draw_row(y, ex, ey);
        if (mode == MODE_QUERY) qy_draw_row(y);
        if (mode == MODE_QUERY) {
            if (y >= qry + 1 && y <= qry + qrh) shade(qrx + qrw, qrx + qrw + 1);
            if (y == qry + qrh) shade(qrx + 2, qrx + qrw + 1);
        }
        /* shadows: to the right of each box, and under it */
        if (mode == MODE_QCD) {
            if (y >= rows - 6 && y <= rows - 3) shade(ax + aw, ax + aw + 1);
            if (y == rows - 2) shade(ax + 2, ax + aw + 1);
        }
        if (mode == MODE_ERR) {
            if (y >= ey + 1 && y <= ey + 8) shade(ex + ewidth, ex + ewidth + 1);
            if (y == ey + 9) shade(ex + 2, ex + ewidth + 1);
        }
        emit_row(y, cols);
    }

    /* cursor: in the quick cd field, otherwise on the command line */
    if (mode == MODE_QCD) {
        cy = rows - 5;
        cx = ax + 6 + line_cells(&qline);
    } else {
        cy = rows - 1;
        cx = prompt_len + line_cells(&cmdl);
        if (cx > cols - 2) cx = cols - 2;
    }
    outs("\x1b[");
    outu((unsigned long)cy + 1);
    outs(";");
    outu((unsigned long)cx + 1);
    outs("H");
    flush();
}


/* ----------------------------------------------------------- start-up */


static void init(void)
{
    struct winsz ws;
    struct timespec ts;
    const char *u;
    long fd, n;

    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.col && ws.row) {
        cols = ws.col;
        rows = ws.row;
    }
    if (cols > MAXW) cols = MAXW;

    if (getcwd(P[0].path, sizeof P[0].path) < 0)
        strlcpy(P[0].path, "/", sizeof P[0].path);
    strlcpy(P[1].path, P[0].path, sizeof P[1].path);

    strlcpy(host, "localhost", sizeof host);
    fd = open("/proc/sys/kernel/hostname", O_RDONLY, 0);
    if (fd >= 0) {
        n = read((int)fd, host, sizeof host - 1);
        close(fd);
        if (n > 0) {
            if (host[n - 1] == '\n') n--;
            host[n] = 0;
        }
    }

    uid = getuid();
    user_name();
    if (!user[0]) {
        u = genv("USER");
        strlcpy(user, u ? u : "user", sizeof user);
    }

    fd = open("/etc/localtime", O_RDONLY, 0);
    if (fd >= 0) {
        tzn = read((int)fd, tzb, sizeof tzb);
        close(fd);
        if (tzn < 0) tzn = 0;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    now = ts.tv_sec;

    load(&P[0]);
    load(&P[1]);
}

/* ---------------------------------------------------------------- main */

/* Signal handler return path. x86_64 kernels need the program to supply the
 * rt_sigreturn sequence (SA_RESTORER); on the other architectures the kernel
 * provides its own, so no restorer is set. */
#if defined(__x86_64__)
__attribute__((naked)) static void restorer(void)
{
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}
#define NEED_RESTORER 1
#endif

/* SIGWINCH needs no work here: the handler only has to exist, so that a
 * blocked read() returns EINTR and the main loop repaints at the new size. */
static void on_winch(int sig)
{
    (void)sig;
}

/* Zero a block without a memcpy call (see tcopy). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void zero_block(void *p, size_t n)
{
    uint8_t *d = (uint8_t *)p;

    for (size_t i = 0; i < n; i++) d[i] = 0;
}

static void install_winch(void)
{
    struct sigaction sa;

    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = on_winch;
    sa.sa_flags = 0;                           /* no SA_RESTART: read() returns EINTR */
#ifdef NEED_RESTORER
    sa.sa_flags |= SA_RESTORER;
    sa.sa_restorer = restorer;
#endif
    rt_sigaction(SIGWINCH, &sa, NULL, 8);
}

/* Struct copy without a memcpy call: the freestanding build has no libc, and
 * some targets would otherwise call an undefined memcpy. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void tcopy(struct ktermios *d, const struct ktermios *s)
{
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;

    for (size_t i = 0; i < sizeof *d; i++) dp[i] = sp[i];
}

/* One key while no dialog is open or the error box is closing. Returns 1 to quit. */
static int handle_key(int key);

/* The first entry, in display order from position FROM on and then round from
 * the top, whose name begins with the N bytes at PREF.  -1 if there is none. */
static int qs_find(const struct panel *p, int from, const char *pref, int n)
{
    if (p->n == 0) return -1;
    for (int j = 0; j < p->n; j++) {
        int i = (from + j) % p->n, k = 0;
        const char *nm = p->nm + p->e[p->ord[i]].name;

        while (k < n && nm[k] == pref[k]) k++;
        if (k == n) return i;
    }
    return -1;
}

/* Quick search (Alt+S): the letters typed are the beginning of a name, and the
 * highlight goes to the first such name from the highlighted one on, round
 * again at the end.  A letter that finds no such name is ignored.  Alt+S again
 * goes on to the next name that begins with the letters.  Esc leaves the search
 * where the highlight is; Backspace takes a letter back.  Any other
 * key leaves the search and is done as it is in the panels: Up and Down move
 * from the highlighted name, Enter and the F keys act on it. */
static int qs_handle(int key)
{
    struct panel *p = &P[act];

    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return 0;
    }
    if (key == (K_ALT | 's')) {                   /* the next name that begins with them */
        int at = qs_find(p, p->cur + 1, qline.buf, qline.len);

        if (at >= 0) {
            p->cur = at;
            scroll(p);
        }
        return 0;
    }
    if (key == 8 || key == 127) {
        if (qline.len) qline.buf[--qline.len] = 0;
        return 0;
    }
    if (key >= 0x20 && key < 0x7f) {
        int at;

        if (qline.len >= 255) return 0;
        qline.buf[qline.len] = (char)key;
        at = qs_find(p, p->cur, qline.buf, qline.len + 1);
        if (at >= 0) {
            qline.len++;
            p->cur = at;
            scroll(p);
        } else {
            qline.buf[qline.len] = 0;
        }
        return 0;
    }
    mode = MODE_NORMAL;
    return handle_key(key);
}

static int handle_key(int key)
{
    struct panel *p = &P[act];

    if (mode == MODE_ERR) {                        /* any key closes the error box */
        mode = MODE_NORMAL;
        return 0;
    }
    if (mode == MODE_QCD) {
        qcd_handle(key);
        return 0;
    }
    if (mode == MODE_QUERY) return qy_handle(key);
    if (mode == MODE_QSEARCH) return qs_handle(key);
    if (key == (K_ALT | '0') || key == K_F10) return 1;
    if (key == (K_ALT | '3') || key == K_F3) {
        view_item(p);
        return 0;
    }
    if (key == (K_ALT | 'c')) {
        mode = MODE_QCD;
        line_clear(&qline);
        return 0;
    }
    if (key == (K_ALT | 's')) {
        mode = MODE_QSEARCH;
        line_clear(&qline);
        return 0;
    }
    if (key == '\r' || key == '\n') {
        if (cmdl.len) run_command();
        else enter_item(p);
        return 0;
    }
    if (key == 9) act ^= 1;                        /* Tab */
    else if (key == 15) toggle_hidden();           /* Ctrl+O */
    else if (key == 18) panel_rescan(p);           /* Ctrl+R: read the directory again */
    else if (key == K_F8 || key == (K_ALT | '8')) delete_open(p);
    else if (key == 21) lft ^= 1;                  /* Ctrl+U */
    else if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN ||
             key == K_HOME || key == K_END)
        nav_key(p, key);
    else if (key == K_LEFT || key == K_RIGHT || key == K_CLEFT || key == K_CRIGHT ||
             key == K_ALTBS || key == K_DEL || key == 8 || key == 127 ||
             (key >= 0x20 && key < 0x7f) || (key >= 0x80 && key < 0x100))
        line_key(&cmdl, key, cmd_width(), 0);
    return 0;
}

__attribute__((noreturn, noinline)) static void main(int argc, char **argv)
{
    unsigned char in[64];
    int quit = 0;
    long n;

    envp = argv + argc + 1;
    init();
    {
        const char *v = genv("LC_ALL");

        if (!v || !*v) v = genv("LC_CTYPE");
        if (!v || !*v) v = genv("LANG");
        utf8_lines = v && has_utf8(v);
    }

    tty_ok = ioctl(0, TCGETS, &tty_saved) == 0;
    if (tty_ok) {
        tcopy(&tty_raw, &tty_saved);
        tty_raw.c_iflag &= ~(IXON | ICRNL);
        tty_raw.c_oflag &= ~OPOST;
        tty_raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        tty_raw.c_cc[VMIN] = 1;
        tty_raw.c_cc[VTIME] = 0;
    }
    install_winch();
    tty_enter();

    while (!quit) {
        struct winsz ws;

        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.col && ws.row &&
            (ws.col != cols || ws.row != rows)) {
            cols = ws.col > MAXW ? MAXW : ws.col;
            rows = ws.row;
            if (sh_fd >= 0) size_pty(sh_fd);
            scroll(&P[0]);
            scroll(&P[1]);
            if (!hidden) outs("\x1b[2J");
        }
        draw();

        if (!wait_term()) continue;          /* SIGWINCH: the loop re-reads the size */
        n = read(0, in, sizeof in);
        if (n == -EINTR) continue;
        if (n <= 0) break;

        for (int i = 0; i < n && !quit; ) {
            int len, key;

            if (hidden) {                    /* the shell view: as typed, but Ctrl+O */
                int j = i;

                while (j < n && in[j] != 15) j++;
                shell_in(in + i, j - i);
                if (j == n) break;
                i = j + 1;
                toggle_hidden();
                continue;
            }
            key = key_parse(in, (int)n, i, &len);
            if (key == K_INCOMPLETE) {
                int m = wait_input(in, (int)n, sizeof in, ESC_WAIT_MS);

                if (m > n) {                   /* the rest of the key arrived */
                    n = m;
                    continue;
                }
                if (in[i] == K_ESC && i + 1 == n) {   /* a lone ESC */
                    key = K_ESC;
                    len = 1;
                } else {
                    break;                     /* a cut-off sequence: drop it */
                }
            }
            i += len;
            quit = handle_key(key);
        }
    }

    tty_leave();
    exit(0);
    __builtin_unreachable();
}
