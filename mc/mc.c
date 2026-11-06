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
 *                      $SHELL -c instead. An archive (.zip, .tar, .tgz,
 *                      .tar.gz, .txz, .tar.xz, .tar.zst, .tar.zstd) is entered
 *                      as a directory, with a title such as archive.zip/utar://
 *                      or archive.zip/uzip://; in it, ".." goes up, and at its
 *                      top, out of the archive. A file in an archive is only
 *                      read out, see vfs_view(): a copy of it is made under
 *                      TMPDIR, for copying or for mcview, and removed after.
 *     F3, ESC 3        mcview on the highlighted file (or enter a directory).
 *                      In an archive, on a copy of the file, made under TMPDIR
 *                      for mcview and removed when it exits.
 *     F5, ESC 5        copy the marked items, or else the highlighted one.
 *                      A question asks for the destination, which starts as the
 *                      other panel's directory; a directory there takes the items
 *                      under their own names. A directory is copied with what is
 *                      in it, and merged into one that exists. A file that exists
 *                      is asked about: Yes, No, Append, or All, Older, None,
 *                      Smaller, Size differs for the rest of the job. A progress
 *                      box follows; Esc (or A) stops the copy, and removes what
 *                      was being written. From an archive, the items are read
 *                      out in one pass over it, into a directory under TMPDIR,
 *                      and copied from there. An archive is read-only: a copy
 *                      into one is refused.
 *     F6, ESC 6        move (rename) the same way. Across file systems, a copy
 *                      and then the removal of the source. Refused in an archive.
 *     F8, ESC 8        delete the marked items, or else the highlighted one, after
 *                      asking. A non-empty directory is asked about again, once
 *                      for each directory of the selection: Yes, No, All (the
 *                      rest), None (the rest), or Abort. Refused in an archive.
 *     Insert           mark the highlighted item (or unmark it), and move down;
 *     Shift+Down/Up    the same, Shift+Up moving up. ".." is never marked.
 *     Ctrl+R           read the active panel's directory again
 *     Alt+C            quick cd: type a directory, Enter changes to it
 *     Alt+Enter        the highlighted name goes on the command line, at the
 *                      cursor, quoted for the shell, and a space after it
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
 * the command line is the same as Alt+C DIR.  A panel is read again when a
 * file has changed, after a command, a viewer, the shell or a file operation, see rescan_changed().  Alt+S is quick search: the info
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
 * The sizes, the colours and the file classes are in config.h.
 *
 * Every frame is repainted in full. The terminal size is re-read when
 * SIGWINCH interrupts the wait for a key, so a resize repaints at once.
 */
#include "../_sys/_main.h"
#include "../_lib/width.h"

#define ESAME 1000           /* not an errno: source and destination are one file */
#define ESUB 1002            /* not an errno: a directory into itself */




/* Cell attributes: bits 0-2 fg, 3-5 bg, 6 bold, 7 DEC line drawing,
 * 8 terminal default colours. */
#define ATTR(fg, bg, b)  ((fg) | ((bg) << 3) | ((b) << 6))
#define DEC   0x080
#define DEFA  0x100
#define BRIGHT 0x200         /* bright foreground colour (SGR 9x) */
#include "config.h"

#define ERR_H  (7 + 2 * DIALOG_PAD)  /* rows of the error box */
#define QCD_H  (3 + 2 * DIALOG_PAD)  /* rows of the quick cd box */
#define OPFW   (opw - 2 * DIALOG_PAD) /* width of a copy, move or question box's frame */

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
    uint32_t sel  : 1;           /* marked for a group operation (Insert, Shift+arrows) */
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

struct archive;
static long join(char *dst, size_t max, const char *dir, const char *name);

/* The kinds of archive, by their names (archive_kind). */
enum { AR_NONE, AR_ZIP, AR_TAR, AR_GZ, AR_XZ, AR_ZST };
static int archive_kind(const char *name);

struct panel {
    char     path[4096];         /* the directory; inside an archive, the one holding it */
    char     arc[256];           /* the archive shown as a directory, "" if none */
    char     sub[256];           /* the directory inside the archive: "a/b/", "" at its root */
    struct archive *ar;          /* the archive's index, while arc is set */
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
enum { MODE_NORMAL, MODE_QCD, MODE_ERR, MODE_QSEARCH, MODE_QUERY, MODE_OP, MODE_PROG,
       MODE_CONFLICT };
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

/* The path at the top of a panel, as drawn. In an archive it is the archive's name
 * and the directory inside it, as mc's uzip shows: "archive.zip/uzip://a/b". */
static int panel_title(char *out, int max, const struct panel *p)
{
    if (!p->arc[0]) return disp_path(out, max, p->path);
    strlcpy(out, p->arc, (size_t)max);
    strlcat(out, archive_kind(p->arc) == AR_ZIP ? "/uzip://" : "/utar://", (size_t)max);
    if (p->sub[0]) {
        strlcat(out, p->sub, (size_t)max);
        out[strlen(out) - 1] = 0;               /* no trailing / */
    }
    return (int)strlen(out);
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

/* ------------------------------------------------------------------ archives
 *
 * An archive that is entered as a directory (see enter_item) is read once, into an
 * index of every entry in it: its name, size, date and kind. The directory shown is
 * listed from the index. The index is kept while the archive file is the one it was
 * read from (archive_fresh), so going in and out of directories reads nothing again.
 *
 * The kinds read: .zip from its central directory; .tar from the file itself; .tgz,
 * .tar.gz, .txz, .tar.xz, .tar.zst and .tar.zstd from the output of gzip, xz or zstd
 * (-dc) through a pipe. A compressed tar has no table of contents, so its headers are
 * read in one pass over the stream, and the data of each entry is skipped. Nothing is
 * written to disk.
 *
 * A file of an archive is written out only to be shown (F3) or copied (F5): into a
 * directory of its own under TMPDIR, which is removed afterwards. Nothing is ever
 * written into an archive, which is read-only. */

/* The kinds of entry, in an index. */
enum { AE_FILE, AE_DIR, AE_LINK, AE_HARD };

/* One entry of an archive, in its index. */
struct aent {
    uint32_t name;                      /* offset of its name in the index's names */
    uint32_t tgt;                       /* for a link: offset of its target, plus 1; else 0 */
    uint32_t size;                      /* size_pack() */
    uint32_t mtime;                     /* seconds since 1970 */
    uint32_t kind;                      /* AE_* */
};

/* An archive as it was read: its index, and the file it was read from. */
struct archive {
    size_t      map;                    /* bytes mapped, for munmap */
    uint64_t    size, ino;              /* the archive file, as it was read */
    int64_t     mtime, mnsec;
    uint32_t    n, nn;                  /* entries, and bytes of names, in use */
    struct aent *e;
    char        *nm;
};

/* A new, empty index. Its memory is anonymous, and it costs little until it fills. */
static struct archive *archive_new(void)
{
    size_t hdr = (sizeof(struct archive) + 15) & ~(size_t)15;
    size_t total = hdr + AR_MAXE * sizeof(struct aent) + AR_NPOOL;
    uint8_t *m = mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct archive *a;

    if (m == MAP_FAILED) return NULL;
    a = (struct archive *)m;
    a->map = total;
    a->e = (struct aent *)(m + hdr);
    a->nm = (char *)(m + hdr + AR_MAXE * sizeof(struct aent));
    return a;
}

static void archive_free(struct archive *a)
{
    if (a) munmap(a, a->map);
}

/* Leave the archive, if the panel is in one: its index is freed. */
static void vfs_forget(struct panel *p)
{
    archive_free(p->ar);
    p->ar = NULL;
    p->arc[0] = 0;
    p->sub[0] = 0;
}

/* The archive file of panel P, as a path. */
static void archive_path(char *full, size_t max, const struct panel *p)
{
    strlcpy(full, p->path, max);
    strlcat(full, "/", max);
    strlcat(full, p->arc, max);
}

/* A member's name, as it is kept: leading "/" and "./" dropped, and a trailing "/"
 * makes it a directory (*dir). Its length, without that "/", is *n. NULL if it cannot
 * be kept: empty, or longer than 255 bytes. */
static const char *norm_name(const char *name, size_t *n, int *dir)
{
    for (;;) {
        if (name[0] == '/') name++;
        else if (name[0] == '.' && name[1] == '/') name += 2;
        else break;
    }
    *n = strlen(name);
    *dir = 0;
    if (*n && name[*n - 1] == '/') {
        *dir = 1;
        (*n)--;
    }
    return *n == 0 || *n > 255 ? NULL : name;
}

/* Add an entry to the index A. NAME is its path inside the archive (see norm_name).
 * TARGET is a link's target, if it has one. -1 if the index is full. */
static int archive_add(struct archive *a, const char *name, uint64_t size, uint64_t mtime,
                       int kind, const char *target)
{
    size_t n, tn = 0, k;
    int dir, withtgt = 0;
    struct aent *e;

    name = norm_name(name, &n, &dir);
    if (!name) return 0;
    if (dir) kind = AE_DIR;
    if ((kind == AE_LINK || kind == AE_HARD) && target && strlen(target) <= 255) {
        withtgt = 1;
        tn = strlen(target);
    }
    if (a->n >= AR_MAXE || a->nn + n + 1 + (withtgt ? tn + 1 : 0) > AR_NPOOL) return -1;

    for (k = 0; k < n; k++) a->nm[a->nn + k] = name[k];
    a->nm[a->nn + n] = 0;
    e = &a->e[a->n++];
    e->name = a->nn;
    e->tgt = 0;
    a->nn += (uint32_t)n + 1;
    if (withtgt) {
        for (k = 0; k < tn; k++) a->nm[a->nn + k] = target[k];
        a->nm[a->nn + tn] = 0;
        e->tgt = a->nn + 1;
        a->nn += (uint32_t)tn + 1;
    }
    e->size = size_pack(size);
    e->mtime = (uint32_t)mtime;
    e->kind = (uint32_t)kind;
    return 0;
}

/* The entry NM of an archive, if it is inside the directory p->sub, is listed in the
 * panel: as a file, a link or a directory, once however many entries are in it. */
static void vfs_add(struct panel *p, const char *nm, uint32_t size, uint32_t mtime, int kind)
{
    size_t sl = strlen(p->sub), k, l;
    const char *rest, *slash;
    char c[256];
    struct ent *e;
    int at, isdir;

    for (k = 0; k < sl; k++)
        if (nm[k] != p->sub[k]) return;
    rest = nm + sl;
    if (!*rest) return;
    slash = rest;
    while (*slash && *slash != '/') slash++;
    l = (size_t)(slash - rest);
    if (l == 0 || l > 255) return;
    for (k = 0; k < l; k++) c[k] = rest[k];
    c[l] = 0;
    if (!scmp(c, ".") || !scmp(c, "..")) return;
    isdir = *slash == '/' || kind == AE_DIR;    /* more names follow it, or it is one itself */
    if (isdir) {
        for (int q = 1; q < p->ne; q++)
            if (p->e[q].cls == CL_DIR && !scmp(p->nm + p->e[q].name, c)) return;
    }
    if (p->ne >= MAXE) return;
    at = add_name(p, c);
    if (at < 0) return;
    e = &p->e[p->ne++];
    e->name = (uint32_t)at;
    e->up = 0;
    e->sel = 0;
    e->link = !isdir && kind == AE_LINK;
    e->mtime = mtime;
    if (isdir) {
        e->cls = CL_DIR;
        e->size = 0;
    } else {
        e->cls = classify(c, 0);
        e->size = size;
    }
}

/* Every entry of the archive is listed, as the directory p->sub of it. */
static void archive_list(struct panel *p)
{
    const struct archive *a = p->ar;

    for (uint32_t i = 0; a && i < a->n; i++)
        vfs_add(p, a->nm + a->e[i].name, a->e[i].size, a->e[i].mtime, (int)a->e[i].kind);
}

/* The target of the link NAME, in the directory p->sub of the archive shown, into TG.
 * 0 if there is none. */
static int vfs_target(const struct panel *p, const char *name, char *tg, size_t max)
{
    const struct archive *a = p->ar;
    char full[512];

    if (!a) return 0;
    strlcpy(full, p->sub, sizeof full);
    strlcat(full, name, sizeof full);
    for (uint32_t i = 0; i < a->n; i++) {
        const struct aent *x = &a->e[i];

        if (x->kind == AE_LINK && x->tgt && !scmp(a->nm + x->name, full)) {
            strlcpy(tg, a->nm + x->tgt - 1, max);
            return 1;
        }
    }
    return 0;
}

/* The kind of archive NAME is, by its name, in any case: AR_NONE if it is not one. */
static int archive_kind(const char *name)
{
    static const struct { const char *ext; uint8_t kind; } ar[] = {
        { ".zip", AR_ZIP },
        { ".tar", AR_TAR },
        { ".tgz", AR_GZ },    { ".tar.gz", AR_GZ },
        { ".txz", AR_XZ },    { ".tar.xz", AR_XZ },
        { ".tar.zst", AR_ZST }, { ".tar.zstd", AR_ZST },
    };
    size_t n = strlen(name);

    for (size_t i = 0; i < sizeof ar / sizeof ar[0]; i++) {
        size_t l = strlen(ar[i].ext), k;

        if (n <= l) continue;
        for (k = 0; k < l && lc(name[n - l + k]) == ar[i].ext[k]; k++)
            ;
        if (k == l) return ar[i].kind;
    }
    return AR_NONE;
}

/* Read the input FD up to N bytes, unless it ends first: the number read. */
static size_t read_full(long fd, void *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        long r = read((int)fd, (char *)buf + got, n - got);

        if (r == -EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}

/* ---- inflate: the raw deflate stream (RFC 1951) of a zip member, written out in
 * pieces. The last INF_WIN bytes written are kept, as a back reference may reach
 * that far. */
#define INF_WIN 32768u
#define INF_OUT 65536u

struct huff { uint16_t count[16]; uint16_t sym[288]; };

struct infl {
    const uint8_t *in;
    size_t n, i;                /* the input, and the next byte of it */
    uint32_t bb;                /* the byte being read: its bits not used yet */
    int nb;                     /* how many of them are left */
    uint8_t out[INF_OUT];
    size_t pos;                 /* bytes of out in use */
    uint64_t total;             /* bytes written so far, in all */
    long err;                   /* a negative errno, once the stream is damaged */
    long (*put)(void *, const uint8_t *, size_t);
    void *arg;
};

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2,
    14, 1, 15 };

static void inf_put(struct infl *s, const uint8_t *p, size_t n)
{
    if (!s->err && n) {
        long r = s->put(s->arg, p, n);

        if (r < 0) s->err = r;
    }
}

/* The next NEED bits of the input, the first one least significant. */
static uint32_t inf_bits(struct infl *s, int need)
{
    uint32_t v = 0;

    for (int k = 0; k < need; k++) {
        if (!s->nb) {
            if (s->i >= s->n) {
                s->err = -EIO;
                return 0;
            }
            s->bb = s->in[s->i++];
            s->nb = 8;
        }
        v |= (s->bb & 1) << k;
        s->bb >>= 1;
        s->nb--;
    }
    return v;
}

/* Writes out all of the window but its last INF_WIN bytes, which stay. */
static void inf_slide(struct infl *s)
{
    size_t keep = s->pos - INF_WIN, k;

    inf_put(s, s->out, keep);
    for (k = 0; k < INF_WIN; k++) s->out[k] = s->out[keep + k];
    s->pos = INF_WIN;
}

static void inf_byte(struct infl *s, uint8_t c)
{
    if (s->pos == INF_OUT) inf_slide(s);
    s->out[s->pos++] = c;
    s->total++;
}

/* The canonical Huffman code of the N lengths in LEN, for decoding. */
static void huff_build(struct huff *h, const uint8_t *len, int n)
{
    uint16_t offs[16];

    for (int l = 0; l < 16; l++) h->count[l] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

/* One symbol of the code H, read bit by bit. */
static int huff_decode(struct infl *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        int count;

        code |= (int)inf_bits(s, 1);
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = -EINVAL;
    return 0;
}

static void inf_codes(struct infl *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym, len, d;
        uint32_t dist;

        sym = huff_decode(s, lc);
        if (s->err) return;
        if (sym < 256) {
            inf_byte(s, (uint8_t)sym);
            continue;
        }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) {
            s->err = -EINVAL;
            return;
        }
        len = lbase[sym] + (int)inf_bits(s, lext[sym]);
        d = huff_decode(s, dc);
        if (s->err) return;
        if (d >= 30) {
            s->err = -EINVAL;
            return;
        }
        dist = dbase[d] + inf_bits(s, dext[d]);
        if (s->err) return;
        if (dist > s->total) {
            s->err = -EINVAL;
            return;
        }
        while (len--) inf_byte(s, s->out[s->pos - dist]);
    }
}

/* A block stored as it is: its length is checked against its complement. */
static void inf_stored(struct infl *s)
{
    uint32_t len, nlen;

    s->nb = 0;                          /* the rest of the byte is not used */
    if (s->i + 4 > s->n) {
        s->err = -EIO;
        return;
    }
    len = s->in[s->i] | s->in[s->i + 1] << 8;
    nlen = s->in[s->i + 2] | s->in[s->i + 3] << 8;
    s->i += 4;
    if (len != (~nlen & 0xffffu)) {
        s->err = -EINVAL;
        return;
    }
    if (len > s->n - s->i) {
        s->err = -EIO;
        return;
    }
    while (len--) inf_byte(s, s->in[s->i++]);
}

/* A block with the fixed codes of the format. */
static void inf_fixed(struct infl *s)
{
    uint8_t len[288 + 30];
    struct huff lc, dc;
    int i;

    for (i = 0; i < 144; i++) len[i] = 8;
    for (; i < 256; i++) len[i] = 9;
    for (; i < 280; i++) len[i] = 7;
    for (; i < 288; i++) len[i] = 8;
    huff_build(&lc, len, 288);
    for (i = 0; i < 30; i++) len[i] = 5;
    huff_build(&dc, len, 30);
    inf_codes(s, &lc, &dc);
}

/* A block with codes of its own, sent first. */
static void inf_dynamic(struct infl *s)
{
    uint8_t len[320];
    struct huff cl, lc, dc;
    int nlen, ndist, ncode, index = 0;

    nlen = (int)inf_bits(s, 5) + 257;
    ndist = (int)inf_bits(s, 5) + 1;
    ncode = (int)inf_bits(s, 4) + 4;
    if (s->err) return;
    if (nlen > 286 || ndist > 30) {
        s->err = -EINVAL;
        return;
    }
    for (int i = 0; i < 19; i++) len[i] = 0;
    for (int i = 0; i < ncode; i++) len[clorder[i]] = (uint8_t)inf_bits(s, 3);
    huff_build(&cl, len, 19);
    while (index < nlen + ndist) {
        int sym, rep, val = 0;

        sym = huff_decode(s, &cl);
        if (s->err) return;
        if (sym < 16) {
            len[index++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!index) {
                s->err = -EINVAL;
                return;
            }
            val = len[index - 1];
            rep = 3 + (int)inf_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)inf_bits(s, 3);
        } else {
            rep = 11 + (int)inf_bits(s, 7);
        }
        if (index + rep > nlen + ndist) {
            s->err = -EINVAL;
            return;
        }
        while (rep--) len[index++] = (uint8_t)val;
    }
    if (len[256] == 0) {
        s->err = -EINVAL;
        return;
    }
    huff_build(&lc, len, nlen);
    huff_build(&dc, len + nlen, ndist);
    inf_codes(s, &lc, &dc);
}

/* Inflates the raw deflate stream IN, N bytes, writing its output through PUT (which
 * returns 0 or a negative errno). *TOTAL, if given, is the output's size. 0, or a
 * negative errno. */
static long inflate_raw(const uint8_t *in, size_t n,
                        long (*put)(void *, const uint8_t *, size_t), void *arg,
                        uint64_t *total)
{
    static struct infl s;
    int last;

    s.in = in;
    s.n = n;
    s.i = 0;
    s.bb = 0;
    s.nb = 0;
    s.pos = 0;
    s.total = 0;
    s.err = 0;
    s.put = put;
    s.arg = arg;
    do {
        uint32_t type;

        last = (int)inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (s.err) break;
        if (type == 0) inf_stored(&s);
        else if (type == 1) inf_fixed(&s);
        else if (type == 2) inf_dynamic(&s);
        else s.err = -EINVAL;
    } while (!last && !s.err);
    inf_put(&s, s.out, s.pos);
    if (total) *total = s.total;
    return s.err;
}

/* ---- zip */

#define ZIP_EOCD_SIG 0x06054b50u        /* end of central directory record */
#define ZIP_CEN_SIG  0x02014b50u        /* central directory file header */
#define ZIP_LOC_SIG  0x04034b50u        /* local file header */
#define ZIP_EOCD_LEN 22
#define ZIP_CEN_LEN  46

static uint32_t zip16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t zip32(const uint8_t *p) { return zip16(p) | zip16(p + 2) << 16; }

/* Days since 1970-01-01 of a date: the inverse of civil(). */
static int32_t days_of(int32_t y, int32_t m, int32_t d)
{
    int32_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* A zip's DOS date and time, which are local time, as seconds since 1970. */
static uint32_t zip_time(uint32_t dt, uint32_t dd)
{
    int64_t t = (int64_t)days_of(1980 + (int32_t)(dd >> 9), (int32_t)((dd >> 5) & 15),
                                 (int32_t)(dd & 31)) * 86400
              + (int64_t)(dt >> 11) * 3600 + (int64_t)((dt >> 5) & 63) * 60
              + (int64_t)(dt & 31) * 2;
    return (uint32_t)(t - tz_off(t));
}

/* ---- extraction: the members of an archive that are taken, written under a
 * directory of their own (the staging directory). A member is written only where
 * its parent directories are real directories: nothing is written through a
 * symbolic link, and a name with an empty, "." or ".." component is never written. */

/* What an extraction takes, and where it goes. The paths taken are packed in buf,
 * NUL-terminated, and off[] gives where each one is. */
struct xout {
    char     root[4096];        /* the staging directory, "" when there is none */
    uint32_t n;
    uint32_t off[MAXE];
    size_t   used;
    char     buf[NPOOL];
    long     err;               /* the first failure: 0 if none */
};
static struct xout xo;

static long xo_add(const char *name)
{
    size_t l = strlen(name);

    if (xo.n >= MAXE || xo.used + l + 1 > NPOOL) return -ENOSPC;
    for (size_t k = 0; k <= l; k++) xo.buf[xo.used + k] = name[k];
    xo.off[xo.n++] = (uint32_t)xo.used;
    xo.used += l + 1;
    return 0;
}

/* Whether NAME is one of the paths taken, or lies inside a directory taken. */
static int xwanted(const struct xout *x, const char *name)
{
    for (uint32_t i = 0; i < x->n; i++) {
        const char *s = x->buf + x->off[i];
        size_t l = strlen(s), k;

        for (k = 0; k < l && name[k] == s[k]; k++)
            ;
        if (k == l && (name[l] == 0 || name[l] == '/')) return 1;
    }
    return 0;
}

/* Whether NAME has an empty, "." or ".." component. */
static int unsafe_name(const char *name)
{
    size_t s = 0;

    for (size_t k = 0;; k++) {
        if (name[k] == '/' || name[k] == 0) {
            size_t l = k - s;

            if (l == 0 || (l == 1 && name[s] == '.') ||
                (l == 2 && name[s] == '.' && name[s + 1] == '.'))
                return 1;
            if (name[k] == 0) return 0;
            s = k + 1;
        }
    }
}

static void xfail(struct xout *x, long r)
{
    if (r && !x->err) x->err = r;
}

/* Whether PATH is a directory, and not a link to one. */
static int is_real_dir(const char *path)
{
    struct statx sx;

    return statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0 &&
           (sx.stx_mode & S_IFMT) == S_IFDIR;
}

/* Makes the directories above NAME under ROOT, one by one. A component that is not
 * a real directory (a file, or a link) stops it: -ENOTDIR. */
static long xparents(const char *root, const char *name)
{
    char path[4352];
    size_t pl, s = 0;

    strlcpy(path, root, sizeof path);
    pl = strlen(path);
    for (size_t k = 0; name[k]; k++) {
        long r;

        if (name[k] != '/') continue;
        path[pl++] = '/';
        for (size_t j = s; j < k; j++) path[pl++] = name[j];
        path[pl] = 0;
        r = mkdirat(AT_FDCWD, path, 0755);
        if (r < 0 && r != -EEXIST) return r;
        if (!is_real_dir(path)) return -ENOTDIR;
        s = k + 1;
    }
    return 0;
}

/* Writes all N bytes at P to FD: 0, or a negative errno. */
static long write_all(long fd, const uint8_t *p, size_t n)
{
    while (n) {
        long r = write((int)fd, p, n);

        if (r == -EINTR) continue;
        if (r < 0) return r;
        if (r == 0) return -EIO;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* A member is written by FILL, which writes its bytes into the file FD, or, when
 * there is no file (FD < 0), only consumes them: a tar stream must move on. */
static long xmember(struct xout *x, const char *name, int mode,
                    long (*fill)(void *, long), void *arg)
{
    char path[4352];
    long fd = -ENAMETOOLONG, r;

    if (!join(path, sizeof path, x->root, name)) {
        r = xparents(x->root, name);
        fd = r ? r : open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, (mode_t)mode);
    }
    r = fill(arg, fd);
    if (fd >= 0) close((int)fd);
    else r = fd;
    if (r) {
        if (fd >= 0) unlinkat(AT_FDCWD, path, 0);
        xfail(x, r);
    }
    return r;
}

static long zdata_put(void *arg, const uint8_t *p, size_t n)
{
    return write_all(*(long *)arg, p, n);
}

/* A zip member's data in the archive: stored, or deflated. */
struct zdata { const uint8_t *p; uint64_t n, usize; int method; };

static long zdata_fill(void *arg, long fd)
{
    const struct zdata *z = arg;
    uint64_t total = 0;
    long r;

    if (fd < 0) return fd;
    if (z->method == 0) return z->n == z->usize ? write_all(fd, z->p, z->n) : -EINVAL;
    r = inflate_raw(z->p, z->n, zdata_put, &fd, &total);
    if (r) return r;
    return total == z->usize ? 0 : -EINVAL;
}

/* A tar entry's data: the next SIZE bytes of the stream IN. */
struct tdata { long in; uint64_t size; };

static long tdata_fill(void *arg, long fd)
{
    const struct tdata *t = arg;
    uint8_t buf[4096];
    uint64_t left = t->size;
    long r = fd < 0 ? fd : 0;

    while (left) {
        size_t k = left < sizeof buf ? (size_t)left : sizeof buf;

        if (read_full(t->in, buf, k) != k) return -EIO;
        if (!r) r = write_all(fd, buf, k);
        left -= k;
    }
    return r;
}

/* A hard link's content: a copy of the file it is linked to, already written. The
 * source is not followed through a link: O_NOFOLLOW on its last part. */
static long hard_fill(void *arg, long fd)
{
    const char *src = arg;
    uint8_t buf[4096];
    long in, r = fd < 0 ? fd : 0;

    if (fd < 0) return fd;
    in = open(src, O_RDONLY | O_NOFOLLOW, 0);
    if (in < 0) return in;
    for (;;) {
        long k = read((int)in, buf, sizeof buf);

        if (k == -EINTR) continue;
        if (k < 0) {
            r = k;
            break;
        }
        if (k == 0) break;
        if (!r) r = write_all(fd, buf, (size_t)k);
    }
    close((int)in);
    return r;
}

static void xdir(struct xout *x, const char *name)
{
    char path[4352];
    long r = join(path, sizeof path, x->root, name);

    if (!r) r = xparents(x->root, name);
    if (!r) r = mkdirat(AT_FDCWD, path, 0755);
    if (r == -EEXIST && is_real_dir(path)) r = 0;
    xfail(x, r);
}

static void xlink(struct xout *x, const char *name, const char *target)
{
    char path[4352];
    long r = join(path, sizeof path, x->root, name);

    if (!r) r = xparents(x->root, name);
    if (!r) r = symlinkat(target, AT_FDCWD, path);
    xfail(x, r);
}

static void xhard(struct xout *x, const char *name, const char *target)
{
    char src[4352];
    size_t n;
    int dir;
    const char *t = target ? norm_name(target, &n, &dir) : NULL;
    long r = !t || unsafe_name(t) ? -EINVAL : join(src, sizeof src, x->root, t);

    if (!r) r = xparents(x->root, t);
    if (r) {
        xfail(x, r);
        return;
    }
    xmember(x, name, 0644, hard_fill, src);
}

/* A member of a zip archive, from its central directory record. */
struct zrec {
    const uint8_t *m;           /* the archive, mapped */
    uint64_t size;              /* its size */
    const uint8_t *name;        /* the name, not NUL-terminated */
    uint32_t nl, method, csize, usize, mtime, mode, off;
};

/* Calls CB for each record in the central directory of the zip file FD, SIZE bytes,
 * until CB returns non-zero. 0, or a negative errno: -EINVAL if it is not a zip that
 * can be read (zip64 is not), -EIO if it cannot be mapped. */
static long zip_walk(int fd, uint64_t size, long (*cb)(void *, const struct zrec *),
                     void *ctx)
{
    const uint8_t *m;
    uint64_t cdoff, cdend, pos;
    uint32_t count, k;
    int64_t i;

    if (size < ZIP_EOCD_LEN || size > (uint64_t)(size_t)-1) return -EINVAL;
    m = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) return -EIO;

    /* The end record: the last one of the file, or before a comment of up to 64 KiB,
     * whose comment length must reach exactly to the end of the file. */
    for (i = (int64_t)size - ZIP_EOCD_LEN;
         i >= 0 && i >= (int64_t)size - ZIP_EOCD_LEN - 65535; i--)
        if (zip32(m + i) == ZIP_EOCD_SIG &&
            (uint64_t)i + ZIP_EOCD_LEN + zip16(m + i + 20) == size) break;
    if (i < 0) {
        munmap((void *)m, (size_t)size);
        return -EINVAL;
    }
    count = zip16(m + i + 10);
    cdoff = zip32(m + i + 16);
    cdend = cdoff + zip32(m + i + 12);
    if (cdend > (uint64_t)i || count == 0xffff || cdoff == 0xffffffffu) {
        munmap((void *)m, (size_t)size);        /* zip64, or damaged */
        return -EINVAL;
    }

    for (k = 0, pos = cdoff; k < count && pos + ZIP_CEN_LEN <= cdend; k++) {
        const uint8_t *h = m + pos;
        struct zrec z;
        uint64_t len;

        if (zip32(h) != ZIP_CEN_SIG) break;
        z.nl = zip16(h + 28);
        len = ZIP_CEN_LEN + z.nl + zip16(h + 30) + zip16(h + 32);
        if (pos + len > cdend) break;
        z.m = m;
        z.size = size;
        z.name = h + ZIP_CEN_LEN;
        z.method = zip16(h + 10);
        z.csize = zip32(h + 20);
        z.usize = zip32(h + 24);
        z.mtime = zip_time(zip16(h + 12), zip16(h + 14));
        z.mode = zip32(h + 38) >> 16;
        z.off = zip32(h + 42);
        if (cb(ctx, &z)) break;
        pos += len;
    }
    munmap((void *)m, (size_t)size);
    return 0;
}

/* The index: each record is added. Stops when the index is full. */
static long zip_index_cb(void *ctx, const struct zrec *z)
{
    struct archive *a = ctx;
    char nb[256];

    if (z->nl > 255) return 0;
    for (uint32_t j = 0; j < z->nl; j++) nb[j] = (char)z->name[j];
    nb[z->nl] = 0;
    return archive_add(a, nb, z->usize, z->mtime, AE_FILE, NULL) < 0;
}

/* The extraction: a record taken is written, its data stored or inflated. */
static long zip_extract_cb(void *ctx, const struct zrec *z)
{
    struct xout *x = ctx;
    char nb[256];
    const char *name;
    size_t n;
    int dir;

    if (z->nl > 255) return 0;
    for (uint32_t j = 0; j < z->nl; j++) nb[j] = (char)z->name[j];
    nb[z->nl] = 0;
    name = norm_name(nb, &n, &dir);
    if (!name || !xwanted(x, name) || unsafe_name(name)) return 0;
    if (dir) {
        xdir(x, name);
        return 0;
    }
    if ((uint64_t)z->off + 30 > z->size || zip32(z->m + z->off) != ZIP_LOC_SIG) {
        xfail(x, -EINVAL);
        return 0;
    }
    if (z->method != 0 && z->method != 8) {
        xfail(x, -EINVAL);
        return 0;
    }
    {
        const uint8_t *h = z->m + z->off;
        uint64_t data = (uint64_t)z->off + 30 + zip16(h + 26) + zip16(h + 28);
        struct zdata zd;

        if (data > z->size || z->csize > z->size - data) {
            xfail(x, -EINVAL);
            return 0;
        }
        zd.p = z->m + data;
        zd.n = z->csize;
        zd.usize = z->usize;
        zd.method = (int)z->method;
        xmember(x, name, z->mode & 0777 ? (int)(z->mode & 0777) : 0644, zdata_fill, &zd);
    }
    return 0;
}

#define TAR_BLK 512

static void zero_block(void *p, size_t n);

/* A number of N bytes in a tar header: octal, or the GNU form (base-256) for big
 * values. *bad is set for anything else. */
static uint64_t tar_num(const uint8_t *p, int n, int *bad)
{
    uint64_t v = 0;
    int i = 0;

    if (p[0] & 0x80) {
        v = p[0] & 0x7f;
        for (i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    while (i < n && p[i] >= '0' && p[i] <= '7') v = v << 3 | (uint64_t)(p[i++] - '0');
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    if (i < n) *bad = 1;
    return v;
}

/* A header of zeros is the end of the archive. */
static int tar_zero(const uint8_t *h)
{
    for (int i = 0; i < TAR_BLK; i++)
        if (h[i]) return 0;
    return 1;
}

/* The checksum of a header: its bytes added up, the checksum field counting as spaces. */
static int tar_sum_ok(const uint8_t *h)
{
    uint32_t sum = 0;
    int bad = 0;

    for (int i = 0; i < TAR_BLK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return tar_num(h + 148, 8, &bad) == sum && !bad;
}

/* Skip N bytes of the stream FD: a seek where FD is a file, else read and drop them. */
static int tar_skip(long fd, uint64_t n)
{
    char buf[4096];

    if (lseek((int)fd, 0, SEEK_CUR) >= 0) {
        while (n) {
            uint64_t k = n < (1u << 30) ? n : (1u << 30);

            if (lseek((int)fd, (long)k, SEEK_CUR) < 0) return -1;
            n -= k;
        }
        return 0;
    }
    while (n) {
        size_t k = n < sizeof buf ? (size_t)n : sizeof buf;

        if (read_full(fd, buf, k) != k) return -1;
        n -= k;
    }
    return 0;
}

/* What the headers before an entry say about it: a long name or link (GNU L and K,
 * or pax path and linkpath), a size and a date (pax). The flags are 1 when set, and 2
 * when the text is too long to keep: then the entry is not listed. */
struct tar_meta {
    char     gname[256];                /* GNU L: the name of the next entry */
    char     pname[256];                /* pax path */
    char     lname[256];                /* GNU K, or pax linkpath: a link's target */
    int      hg, hp, hk;
    uint64_t psize, pmtime;
    int      hs, hm;
};

/* The text of N bytes at B, up to a NUL, copied to OUT (*flag is 1), or too long (2). */
static void tar_text(const uint8_t *b, uint64_t n, char *out, int *flag)
{
    uint64_t k = 0;

    while (k < n && b[k]) k++;
    if (k > 255) {
        *flag = 2;
        return;
    }
    for (uint64_t j = 0; j < k; j++) out[j] = (char)b[j];
    out[k] = 0;
    *flag = 1;
}

/* Whether the N bytes at S are the keyword KW. */
static int tar_kw(const uint8_t *s, uint64_t n, const char *kw)
{
    for (uint64_t k = 0; k < n; k++)
        if (!kw[k] || kw[k] != (char)s[k]) return 0;
    return kw[n] == 0;
}

/* A decimal number at the start of N bytes at B, to *out. 0 if there is none. */
static int tar_dec(const uint8_t *b, uint64_t n, uint64_t *out)
{
    uint64_t v = 0, k = 0;

    if (!n || b[0] < '0' || b[0] > '9') return 0;
    while (k < n && b[k] >= '0' && b[k] <= '9') v = v * 10 + (uint64_t)(b[k++] - '0');
    *out = v;
    return 1;
}

/* The records of a pax extended header, B, N bytes: "LEN KEY=VALUE" and a newline,
 * one after another. The keys path, linkpath, size and mtime are taken. */
static void tar_pax(const uint8_t *b, uint64_t n, struct tar_meta *m)
{
    uint64_t i = 0;

    while (i < n) {
        uint64_t len = 0, k = 0, kb, e, vs, ve;

        while (i + k < n && b[i + k] >= '0' && b[i + k] <= '9')
            len = len * 10 + (uint64_t)(b[i + k++] - '0');
        if (i + k >= n || k == 0 || len == 0 || len > n - i || b[i + k] != ' ') return;

        kb = i + k + 1;                     /* the key starts after the space */
        e = kb;
        while (e < i + len && b[e] != '=') e++;
        if (e >= i + len) return;
        vs = e + 1;
        ve = i + len - 1;                   /* the value ends before the newline */
        if (ve < vs) ve = vs;

        if (tar_kw(b + kb, e - kb, "path")) {
            tar_text(b + vs, ve - vs, m->pname, &m->hp);
        } else if (tar_kw(b + kb, e - kb, "linkpath")) {
            tar_text(b + vs, ve - vs, m->lname, &m->hk);
        } else if (tar_kw(b + kb, e - kb, "size")) {
            m->hs = tar_dec(b + vs, ve - vs, &m->psize);
        } else if (tar_kw(b + kb, e - kb, "mtime")) {
            m->hm = tar_dec(b + vs, ve - vs, &m->pmtime);
        }
        i += len;
    }
}

/* The name of the ustar header H: its prefix, a "/", and its name, into OUT. 0 if it
 * is too long to keep. The prefix is only there in the POSIX form ("ustar" and NUL),
 * which the GNU form does not share. */
static int tar_ustar(const uint8_t *h, char *out)
{
    static const char magic[6] = { 'u', 's', 't', 'a', 'r', 0 };
    size_t k = 0, i;
    int posix = 1;

    for (i = 0; i < 6; i++)
        if ((char)h[257 + i] != magic[i]) posix = 0;
    if (posix) {
        for (i = 0; i < 155 && h[345 + i]; i++)
            ;
        if (i) {
            if (i + 1 > 255) return 0;
            for (k = 0; k < i; k++) out[k] = (char)h[345 + k];
            out[k++] = '/';
        }
    }
    for (i = 0; i < 100 && h[i]; i++)
        ;
    if (k + i > 255) return 0;
    for (size_t j = 0; j < i; j++) out[k + j] = (char)h[j];
    out[k + i] = 0;
    return 1;
}

/* An entry of a tar stream, taken: a file's data is read from FD and written, the rest
 * is written as it is. Returns the bytes of data read; 0 for an entry not taken, whose
 * data the caller passes over. */
static uint64_t xtar_entry(struct xout *x, long fd, const char *name, int kind, int mode,
                           uint64_t size, const char *target)
{
    size_t n;
    int dir;
    const char *nn = norm_name(name, &n, &dir);
    struct tdata td;

    if (!nn || !xwanted(x, nn) || unsafe_name(nn)) return 0;
    if (dir || kind == AE_DIR) {
        xdir(x, nn);
        return 0;
    }
    if (kind == AE_LINK) {
        xlink(x, nn, target);
        return 0;
    }
    if (kind == AE_HARD) {
        xhard(x, nn, target);
        return 0;
    }
    td.in = fd;
    td.size = size;
    xmember(x, nn, mode ? mode : 0644, tdata_fill, &td);
    return size;
}

/* Reads the tar stream FD, header after header, and passes over the data of each
 * entry. With A, each entry is indexed; with X, the entries taken are extracted. PID
 * is the process writing the stream, or 0 if FD is the file itself. If the index
 * fills, *full is set and the rest is not read. 0, or -EINVAL if the stream is not
 * tar. */
static int tar_scan(struct archive *a, long fd, long pid, int *full, struct xout *x)
{
    uint8_t h[TAR_BLK];
    char lbuf[4096], nm[256], tg[256];
    struct tar_meta m;
    int bad = 0, eoa = 0, flag;

    *full = 0;
    zero_block(&m, sizeof m);
    for (;;) {
        uint64_t size, mtime, eff, pad, blk, used = 0;
        int t, kind, mode, r = 0;

        if (read_full(fd, h, TAR_BLK) != TAR_BLK) break;
        if (tar_zero(h)) {
            eoa = 1;
            break;
        }
        if (!tar_sum_ok(h)) {
            bad = 1;
            break;
        }
        size = tar_num(h + 124, 12, &bad);
        mtime = tar_num(h + 136, 12, &bad);
        if (bad) break;
        t = h[156];
        blk = (size + TAR_BLK - 1) & ~(uint64_t)(TAR_BLK - 1);

        if (t == 'x' || t == 'g' || t == 'L' || t == 'K') {
            if (size <= sizeof lbuf) {
                if (read_full(fd, lbuf, size) != size) break;
                if (tar_skip(fd, blk - size)) break;
                if (t == 'x') tar_pax((uint8_t *)lbuf, size, &m);
                else if (t == 'L') tar_text((uint8_t *)lbuf, size, m.gname, &m.hg);
                else if (t == 'K') tar_text((uint8_t *)lbuf, size, m.lname, &m.hk);
            } else {
                if (tar_skip(fd, blk)) break;
                if (t == 'x') m.hp = 2;     /* too big to read: its path is unknown */
                else if (t == 'L') m.hg = 2;
                else if (t == 'K') m.hk = 2;
            }
            continue;
        }

        eff = m.hs ? m.psize : size;
        pad = (eff + TAR_BLK - 1) & ~(uint64_t)(TAR_BLK - 1);
        kind = t == '5' ? AE_DIR : t == '2' ? AE_LINK : t == '1' ? AE_HARD : AE_FILE;
        mode = (int)(tar_num(h + 100, 8, &flag) & 0777);
        if (m.hg != 2 && m.hp != 2 && !(kind == AE_LINK && m.hk == 2)) {
            const char *name = nm, *target = NULL;
            int ok = 1;

            if (m.hp) name = m.pname;
            else if (m.hg) name = m.gname;
            else ok = tar_ustar(h, nm);
            if (kind == AE_LINK || kind == AE_HARD) {
                if (m.hk) {
                    target = m.lname;
                } else {
                    tar_text(h + 157, 100, tg, &flag);
                    target = tg;
                }
            }
            if (ok) {
                if (x) used = xtar_entry(x, fd, name, kind, mode, eff, target);
                else r = archive_add(a, name, eff, m.hm ? m.pmtime : mtime, kind, target);
            }
        }
        if (r < 0) {
            *full = 1;
            break;
        }
        if (tar_skip(fd, pad - used)) break;
        zero_block(&m, sizeof m);
    }
    if (pid && !*full)
        while (read_full(fd, lbuf, sizeof lbuf) == sizeof lbuf)
            ;                               /* the rest, so that the process ends cleanly */
    if (a && bad && a->n == 0 && !eoa) return -EINVAL;
    return 0;
}

/* Replace the process by TOOL, found on the PATH, with ARGV: it returns only if no
 * element of the PATH has it. */
static void exec_tool(const char *tool, char *const argv[])
{
    const char *path = genv("PATH");
    size_t tl = strlen(tool);
    char buf[4096];

    if (!path) path = "/usr/bin:/bin";
    for (;;) {
        const char *e = path;
        size_t dl, k;

        while (*e && *e != ':') e++;
        dl = (size_t)(e - path);
        if (dl + tl + 2 <= sizeof buf) {
            for (k = 0; k < dl; k++) buf[k] = path[k];
            if (dl == 0) buf[k++] = '.';        /* an empty element is the current directory */
            buf[k++] = '/';
            for (size_t j = 0; j < tl; j++) buf[k++] = tool[j];
            buf[k] = 0;
            execve(buf, argv, envp);
        }
        if (!*e) return;
        path = e + 1;
    }
}

/* Run TOOL on the file FULL, to decompress it with -dc: its output is a pipe, and its
 * read end is returned, with the process in *pid. A negative errno if it cannot be
 * started. Its input is /dev/null, and its errors go there too. */
static long ar_spawn(const char *full, const char *tool, long *pid)
{
    char *argv[4];
    int pf[2];
    long p, dn, r;

    argv[0] = (char *)tool;
    argv[1] = (char *)"-dc";
    argv[2] = (char *)full;
    argv[3] = NULL;
    r = pipe2(pf, O_CLOEXEC);
    if (r < 0) return r;
    p = clone(SIGCHLD, 0, 0, 0, 0);
    if (p < 0) {
        close(pf[0]);
        close(pf[1]);
        return p;
    }
    if (p == 0) {
        dn = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
        dup2(pf[1], 1);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 2);
        }
        exec_tool(tool, argv);
        exit(127);
    }
    close(pf[1]);
    *pid = p;
    return pf[0];
}

/* Wait for the decompressor PID. Unless the index filled (FULL), a failure of the
 * process is a failure of the archive. */
static int ar_wait(long pid, int full, int rc)
{
    int st = 0;

    while (wait4((int)pid, &st, 0, NULL) == -EINTR)
        ;
    if (!full && rc == 0 && ((st & 0x7f) || ((st >> 8) & 0xff))) return -EIO;
    return rc;
}

/* Reads the archive of panel P in one pass: indexed into A if A is given, else its
 * entries taken into X. 0, or a negative errno. */
static long archive_scan(const struct panel *p, struct archive *a, struct xout *x)
{
    char full[4096 + 260];
    struct statx sx;
    const char *tool;
    long fd, pid = 0, out;
    int kind = archive_kind(p->arc), rc = 0, full_idx = 0;

    archive_path(full, sizeof full, p);
    if (statx(AT_FDCWD, full, 0, STATX_BASIC_STATS, &sx) != 0 ||
        (sx.stx_mode & S_IFMT) != S_IFREG)      /* not a file: open() could block */
        return -EINVAL;
    fd = open(full, O_RDONLY, 0);
    if (fd < 0) return fd;
    /* The index is of this file, and the signature is taken from it. */
    if (statx((int)fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) != 0 ||
        (sx.stx_mode & S_IFMT) != S_IFREG) {
        close((int)fd);
        return -EINVAL;
    }
    if (a) {
        a->size = sx.stx_size;
        a->ino = sx.stx_ino;
        a->mtime = sx.stx_mtime.tv_sec;
        a->mnsec = sx.stx_mtime.tv_nsec;
    }

    switch (kind) {
    case AR_ZIP:
        rc = (int)zip_walk((int)fd, sx.stx_size, a ? zip_index_cb : zip_extract_cb,
                           a ? (void *)a : (void *)x);
        break;
    case AR_TAR:
        rc = tar_scan(a, fd, 0, &full_idx, x);
        break;
    case AR_GZ:
    case AR_XZ:
    case AR_ZST:
        tool = kind == AR_GZ ? "gzip" : kind == AR_XZ ? "xz" : "zstd";
        close((int)fd);
        fd = -1;
        out = ar_spawn(full, tool, &pid);
        if (out < 0) {
            rc = (int)out;
            break;
        }
        rc = tar_scan(a, out, pid, &full_idx, x);
        close((int)out);
        rc = ar_wait(pid, full_idx, rc);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    if (fd >= 0) close((int)fd);
    return rc;
}

/* Read the archive of panel P (P->arc, in P->path) into a new index, which replaces
 * the index there is. 0, or a negative errno: then the index there is kept. */
static int archive_read(struct panel *p)
{
    struct archive *a = archive_new();
    int rc;

    if (!a) return -EIO;
    rc = (int)archive_scan(p, a, NULL);
    if (rc) {
        archive_free(a);
        return rc;
    }
    archive_free(p->ar);
    p->ar = a;
    return 0;
}

/* The temporary directory of an extraction: a new one, under TMPDIR, or /tmp. Its
 * path is xo.root. */
static long xtmp_make(void)
{
    const char *t = genv("TMPDIR");
    char num[32];
    long pid = getpid();

    if (!t || !*t) t = "/tmp";
    for (unsigned k = 0; k < 1000; k++) {
        long r;
        int l;

        strlcpy(xo.root, t, sizeof xo.root);
        strlcat(xo.root, "/mc-", sizeof xo.root);
        l = (int)utoa((unsigned long)pid, num);
        num[l] = 0;
        strlcat(xo.root, num, sizeof xo.root);
        strlcat(xo.root, "-", sizeof xo.root);
        l = (int)utoa(k, num);
        num[l] = 0;
        strlcat(xo.root, num, sizeof xo.root);
        r = mkdirat(AT_FDCWD, xo.root, 0700);
        if (r == 0) return 0;
        if (r != -EEXIST) break;
    }
    xo.root[0] = 0;
    return -EEXIST;
}

/* The targets of the hard links taken are staged too: a hard link is written as a copy
 * of its target, which is then in the staging directory, but is not copied itself. */
static void vfs_hard_targets(const struct panel *p)
{
    const struct archive *a = p->ar;

    for (uint32_t i = 0; a && i < a->n; i++) {
        const struct aent *x = &a->e[i];
        char tg[256];
        const char *t;
        size_t n;
        int dir;

        if (x->kind != AE_HARD || !x->tgt || !xwanted(&xo, a->nm + x->name)) continue;
        strlcpy(tg, a->nm + x->tgt - 1, sizeof tg);
        t = norm_name(tg, &n, &dir);
        if (t) xo_add(t);
    }
}

/* Takes the items of the archive panel P that a job works on (the marked ones, if SEL,
 * else the highlighted one) into a new staging directory, and writes them there in one
 * pass over the archive. The job then copies them from there. 0, or a negative errno. */
static long vfs_stage(const struct panel *p, int sel)
{
    long r;

    xo.n = 0;
    xo.used = 0;
    xo.err = 0;
    r = xtmp_make();
    if (r) return r;
    for (int d = 0; d < p->n; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        char inner[512];

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        strlcpy(inner, p->sub, sizeof inner);
        strlcat(inner, p->nm + e->name, sizeof inner);
        r = xo_add(inner);
        if (r) break;
        if (is_dir(e)) xdir(&xo, inner);
    }
    /* a member that fails is missing from the staging directory: the copy says so */
    if (!r) {
        vfs_hard_targets(p);
        r = archive_scan(p, NULL, &xo);
    }
    return r;
}

/* Whether the index of panel P is of the archive file as it is now. */
static int archive_fresh(const struct panel *p)
{
    char full[4096 + 260];
    struct statx sx;

    if (!p->ar) return 0;
    archive_path(full, sizeof full, p);
    if (statx(AT_FDCWD, full, 0, STATX_BASIC_STATS, &sx) != 0) return 0;
    return sx.stx_size == p->ar->size && sx.stx_ino == p->ar->ino &&
           sx.stx_mtime.tv_sec == p->ar->mtime && sx.stx_mtime.tv_nsec == p->ar->mnsec;
}

/* The "..." entry, always first: its name is at offset 0 of the pool, and its date
 * is the mtime of the parent of the path as mc knows it (lexically, so through a
 * symlink it is not the real parent). Inside an archive, that is the directory the
 * archive is in. */
static void load_up(struct panel *p)
{
    struct ent *e = &p->e[0];
    struct statx sx;

    p->nn = 0;
    add_name(p, "..");
    e->name = 0;
    e->size = 0;
    e->cls = CL_DIR;
    e->up = 1;
    e->sel = 0;
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
}

/* The list: "..." first (not in the root directory, and always inside an archive),
 * then the rest sorted. */
static void sort_list(struct panel *p)
{
    int up = p->arc[0] || !(p->path[0] == '/' && p->path[1] == 0), first = up ? 1 : 0;

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

/* Read the panel's directory again: p->path, or the archive p->arc at the
 * directory p->sub. 0 on success; a negative errno if an archive cannot be read,
 * when the listing is only "...". */
static int load(struct panel *p)
{
    long buf[1024];                 /* 8 KiB, aligned for dirent64 records */
    struct statx sx;
    struct linux_dirent64 *d = NULL;
    struct ent *e;
    long fd, r, off;

    if (p->arc[0]) {
        int rc = 0;

        load_up(p);
        if (!archive_fresh(p)) rc = archive_read(p);
        if (rc == 0) archive_list(p);
        sort_list(p);
        p->free_kb = p->total_kb = 0;               /* as mc: an archive has no free space shown */
        return rc;
    }

    fd = open(p->path, O_RDONLY | O_DIRECTORY, 0);

    load_up(p);

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
            e->sel = 0;
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

    sort_list(p);

    {
        struct statfs f;
        p->free_kb = p->total_kb = 0;
        if (statfs(p->path, &f) == 0 && f.f_bsize > 0) {
            p->free_kb  = ((uint64_t)(uid ? f.f_bavail : f.f_bfree) * (uint64_t)f.f_bsize) >> 10;
            p->total_kb = ((uint64_t)f.f_blocks * (uint64_t)f.f_bsize) >> 10;
        }
    }
    return 0;
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

/* Read the directory of P again. The highlight stays on the same name if it is
 * still there, else on the same position; the marks stay on the names that are
 * still there too. The names of the marked entries wait in ob, which is free
 * between frames, so a marked name past the end of ob is not kept. */
static void panel_rescan(struct panel *p)
{
    char name[256];
    int at = p->cur, i, k = 0, j;

    name[0] = 0;
    if (p->n > 0) strlcpy(name, p->nm + p->e[p->ord[p->cur]].name, sizeof name);
    for (i = 0; i < p->n; i++) {
        const struct ent *e = &p->e[p->ord[i]];
        size_t l = strlen(p->nm + e->name) + 1;

        if (e->sel && !e->up && (size_t)k + l <= sizeof ob) {
            memcpy(ob + k, p->nm + e->name, l);
            k += (int)l;
        }
    }
    if (load(p) != 0 && p->arc[0]) {            /* the archive is gone: its directory */
        vfs_forget(p);
        load(p);
    }
    for (j = 1; j < p->ne && k; j++) {
        const char *en = p->nm + p->e[j].name;
        for (int s = 0; s < k; s += (int)strlen(ob + s) + 1) {
            size_t q = 0;

            while (ob[s + (int)q] && ob[s + (int)q] == en[q]) q++;
            if (!ob[s + (int)q] && !en[q]) {
                p->e[j].sel = 1;
                break;
            }
        }
    }
    p->cur = at < p->n ? at : p->n - 1;
    if (p->cur < 0) p->cur = 0;
    for (i = 0; i < p->n; i++) {
        const char *en = p->nm + p->e[p->ord[i]].name;
        size_t k2 = 0;

        while (name[k2] && name[k2] == en[k2]) k2++;
        if (name[k2] == 0 && en[k2] == 0) {
            p->cur = i;
            break;
        }
    }
    scroll(p);
}

/* After something may have changed the files (a command, a viewer, the shell, a
 * file operation), both panels are read again: a file can change in place, its
 * size and date, without its directory changing at all. The highlight and the
 * marks stay on their names. */
static void rescan_changed(void)
{
    panel_rescan(&P[0]);
    panel_rescan(&P[1]);
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
        int tl = panel_title(pb, sizeof pb, p), tc;
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

            a = hit ? (e->sel ? C_SELCUR : C_CURSOR) : e->sel ? C_SEL : cl_attr[e->cls];

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
            } else if (e->link && p->arc[0]) {      /* in an archive: the target, from its index */
                char tg[256];

                n = cat(t, "-> ");
                if (vfs_target(p, p->nm + e->name, tg, sizeof tg))
                    n += fit_name(t + n, tg, pw - 5);
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

/* The UTF-8 bytes of the code point CP, into B (4 bytes at most); returns their number. */
static int utf8_enc(char *b, uint32_t cp)
{
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
    return n;
}

static void put_utf8(uint32_t cp)
{
    char b[4];

    put(b, utf8_enc(b, cp));
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
static void vfs_open(struct panel *p, const struct ent *e);
static void vfs_view(struct panel *p, const struct ent *e);
static void zero_block(void *p, size_t n);

/* ----------------------------------------------------------- keys and lines */

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
#define K_CHAR   0x100000   /* a character typed: K_CHAR | its code point */
#define K_F8     0x10e
#define K_F5     0x10f
#define K_F6     0x110
#define K_SUP    0x111
#define K_SDOWN  0x112
#define K_INS    0x113
#define K_NONE   0x1ff
#define K_ALT    0x200          /* Alt+x is K_ALT | x */

/* Decode one key at in[i]. Returns the key and sets *len, or K_INCOMPLETE if
 * the buffer ends inside a key. Plain bytes are their own keys. */
/* The key for the bytes at in[i], which is not an Esc: a well-formed UTF-8 character
 * is one key, K_CHAR | its code point, of *len bytes; anything else is its byte, as
 * before. K_INCOMPLETE: a character the end of the input cuts off. */
static int char_key(const unsigned char *in, int n, int i, int *len)
{
    int c = in[i], l, k;
    uint32_t cp;

    if (c < 0xc2 || c > 0xf4) return c;
    l = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
    if (i + l > n) return K_INCOMPLETE;
    cp = c & ((1 << (7 - l)) - 1);
    for (k = 1; k < l; k++) {
        if ((in[i + k] & 0xc0) != 0x80) return c;
        cp = cp << 6 | (in[i + k] & 0x3f);
    }
    if (cp < (l == 2 ? 0x80 : l == 3 ? 0x800 : 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return c;
    *len = l;
    return K_CHAR | (int)cp;
}

static int key_parse(const unsigned char *in, int n, int i, int *len)
{
    int j, k, fin, c, nums[2];

    *len = 1;
    if (in[i] != K_ESC) return char_key(in, n, i, len);
    if (i + 1 >= n) return K_INCOMPLETE;
    if (in[i + 1] == 0x7f || in[i + 1] == 0x08) { *len = 2; return K_ALTBS; }
    if (in[i + 1] == K_ESC) return K_ESC;       /* ESC ESC: the first is Esc, as in mc */
    if (in[i + 1] >= 0x80) return K_ESC;        /* not Alt+a character: the Esc alone */
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
    case 'A': return nums[1] == 2 ? K_SUP : K_UP;
    case 'B': return nums[1] == 2 ? K_SDOWN : K_DOWN;
    case 'C': return nums[1] == 5 ? K_CRIGHT : K_RIGHT;
    case 'D': return nums[1] == 5 ? K_CLEFT : K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'R': return in[i + 1] == 'O' ? K_F3 : K_NONE;
    case '~':
        switch (nums[0]) {
        case 1: case 7:  return K_HOME;
        case 4: case 8:  return K_END;
        case 2:          return K_INS;
        case 3:          return K_DEL;
        case 5:          return K_PGUP;
        case 6:          return K_PGDN;
        case 13:         return K_F3;
        case 15:         return K_F5;
        case 17:         return K_F6;
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
/* A character typed, as its UTF-8 bytes, at the cursor. */
static void line_char(struct line *l, uint32_t cp)
{
    char b[4];
    int n = utf8_enc(b, cp), j;

    if (l->len + n >= (int)sizeof l->buf) return;
    for (j = 0; j < n; j++) line_insert(l, (unsigned char)b[j]);
}

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
        if (key & K_CHAR) {
            line_char(l, (uint32_t)(key & ~K_CHAR));
            break;
        }
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
    if (p->arc[0]) return;                      /* inside an archive: not extracted yet */
    if (archive_kind(p->nm + e->name) != AR_NONE) {
        vfs_open(p, e);
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
    if (p->arc[0]) {
        vfs_view(p, e);
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

/* Alt+Enter: the highlighted name goes on the command line, at the cursor, and
 * a space after it. The name is quoted as mc quotes it: a backslash before each
 * character the shell would read as special, and before a leading ~ or #; a
 * leading - gets ./ in front, so it is not an option. A name that does not fit
 * on the line is not typed at all. */
static void put_name(const struct panel *p)
{
    const struct ent *e;
    const char *s;
    char t[520];
    int n = 0, j;

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    s = p->nm + e->name;
    if (*s == '-') {
        t[n++] = '.';
        t[n++] = '/';
    }
    for (; *s && n < (int)sizeof t - 4; s++) {
        int c = (unsigned char)*s, esc = 0;

        for (const char *q = "\t\r\n \"'\\;?|[]{}<>`!$&*()"; *q; q++)
            if (*q == c) esc = 1;
        if (n == 0 && (c == '~' || c == '#')) esc = 1;
        if (esc) t[n++] = '\\';
        t[n++] = (char)c;
    }
    t[n++] = ' ';
    if (cmdl.len + n >= (int)sizeof cmdl.buf) return;
    for (j = 0; j < n; j++) line_insert(&cmdl, (unsigned char)t[j]);
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

/* Insert, and Shift+Up or Shift+Down: the highlighted item is marked (or unmarked,
 * if it was marked already), and the cursor moves one item: down for Insert and
 * Shift+Down, up for Shift+Up. \"..\" is never marked; the cursor stops at either end. */
static void sel_key(struct panel *p, int key)
{
    if (p->n == 0) return;
    if (!p->e[p->ord[p->cur]].up) p->e[p->ord[p->cur]].sel ^= 1;
    if (key == K_SUP) {
        if (p->cur > 0) p->cur--;
    } else if (p->cur < p->n - 1) {
        p->cur++;
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
    case 17: return "File exists";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
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
    strlcpy(eline[1], shown, sizeof eline[1]);
    l = (int)strlen(eline[1]);
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
    ewidth = w + 4 + 2 * DIALOG_PAD;
    if (ewidth > cols) ewidth = cols;
    mode = MODE_ERR;
}

/* Change the active panel to the directory the user typed. As in mc: blanks
 * around the text are dropped, an empty entry means $HOME, "~" is $HOME, and a
 * relative path is taken from the panel's directory. */
/* The absolute path TYPED names, as quick cd takes it: blanks around it are
 * dropped, an empty entry or ~ is $HOME, ~USER is that account's home, and a
 * relative path is taken from BASE. RES gets the path, SHOWN the path as typed,
 * for the error box. */
static void path_of(const char *typed, const char *base, char *res, size_t rmax,
                    char *shown, size_t smax)
{
    char s[256], abs[4096];
    const char *home = genv("HOME");
    size_t a = 0, b, n = 0;

    b = strlen(typed);
    while (a < b && (typed[a] == ' ' || typed[a] == '\t')) a++;
    while (b > a && (typed[b - 1] == ' ' || typed[b - 1] == '\t' ||
                     typed[b - 1] == '\r' || typed[b - 1] == '\n')) b--;
    for (size_t i = a; i < b; i++) s[n++] = typed[i];
    s[n] = 0;

    if (n == 0) {
        strlcpy(shown, home ? home : "/", smax);
    } else if (s[0] == '~') {                      /* ~ and ~/ : the account's home */
        size_t k = 1;
        char hb[4096];
        int ok;

        while (s[k] && s[k] != '/') k++;
        ok = k == 1 ? pw_field(NULL, 0, uid, 5, hb, sizeof hb)
                    : pw_field(s + 1, k - 1, 0, 5, hb, sizeof hb);
        if (ok) {
            strlcpy(shown, hb, smax);
            strlcat(shown, s + k, smax);
        } else {
            strlcpy(shown, s, smax);
        }
    } else {
        strlcpy(shown, s, smax);
    }

    if (shown[0] == '/') {
        strlcpy(abs, shown, sizeof abs);
    } else {
        strlcpy(abs, base, sizeof abs);
        if (abs[strlen(abs) - 1] != '/') strlcat(abs, "/", sizeof abs);
        strlcat(abs, shown, sizeof abs);
    }
    canon(res, rmax, abs);
}

static void quick_cd(const char *typed)
{
    char res[4096], shown[512];

    path_of(typed, P[act].path, res, sizeof res, shown, sizeof shown);
    change_to(&P[act], res, shown);
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
    vfs_forget(p);                              /* a real directory: out of any archive */
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

/* The highlight on the entry called NAME, if the list has one. */
static void select_name(struct panel *p, const char *name)
{
    p->cur = 0;
    p->top = 0;
    for (int i = 0; i < p->n; i++)
        if (!scmp(p->nm + p->e[p->ord[i]].name, name)) {
            p->cur = i;
            break;
        }
    scroll(p);
}

/* Enter the archive named by E, a file of the panel's directory: the panel shows the
 * archive's root. An archive that cannot be read is an error, and nothing changes. */
static void vfs_open(struct panel *p, const struct ent *e)
{
    char name[256];
    int rc;

    strlcpy(name, p->nm + e->name, sizeof name);    /* load() rewrites the names */
    strlcpy(p->arc, name, sizeof p->arc);
    p->sub[0] = 0;
    rc = load(p);
    if (rc != 0) {
        vfs_forget(p);
        load(p);
        set_error("Cannot open archive", name, -rc);
        return;
    }
    p->cur = 0;
    p->top = 0;
    scroll(p);
}

/* Enter the directory E of the archive shown. */
static void vfs_enter(struct panel *p, const struct ent *e)
{
    char name[256];

    if (strlen(p->sub) + strlen(p->nm + e->name) + 2 > sizeof p->sub) return;
    strlcpy(name, p->nm + e->name, sizeof name);    /* load() rewrites the names */
    strlcat(p->sub, name, sizeof p->sub);
    strlcat(p->sub, "/", sizeof p->sub);
    load(p);
    p->cur = 0;
    p->top = 0;
    scroll(p);
}

/* ".." in an archive: up one directory inside it, its highlight on the directory
 * left; at the archive's root, out of the archive, its highlight on the archive. */
static void vfs_up(struct panel *p)
{
    char name[256];
    size_t n, st;

    if (!p->sub[0]) {
        strlcpy(name, p->arc, sizeof name);
        vfs_forget(p);
        load(p);
        select_name(p, name);
        return;
    }
    n = strlen(p->sub) - 1;                         /* the last / */
    st = n;
    while (st > 0 && p->sub[st - 1] != '/') st--;   /* the directory left is sub[st..n) */
    strlcpy(name, p->sub + st, sizeof name);
    name[n - st] = 0;
    p->sub[st] = 0;
    load(p);
    select_name(p, name);
}

/* Enter on a directory, or on "..": change the panel to it. Files do nothing. */
static void enter_dir(struct panel *p)
{
    const struct ent *e;
    char abs[4096], res[4096];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (!is_dir(e)) return;
    if (p->arc[0]) {                                /* inside an archive */
        if (e->up) vfs_up(p);
        else vfs_enter(p, e);
        return;
    }
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
    int top = rows - 2 - QCD_H, dy = y - top, r = x0 + pw - 1;
    int fl = x0 + DIALOG_PAD, fr = r - DIALOG_PAD, fd, x;
    const char *title = " Quick cd ";

    if (y < top || y >= top + QCD_H) return;
    for (x = x0; x <= r; x++) setc(x, ' ', C_QCD);
    if (dy < DIALOG_PAD || dy >= QCD_H - DIALOG_PAD) return;       /* the margin */
    fd = dy - DIALOG_PAD;
    if (fd == 0 || fd == 2) {                      /* frame, titled on top */
        setc(fl, fd == 0 ? 'l' : 'm', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, fd == 0 ? 'k' : 'j', C_QCD | DEC);
        if (fd == 0) sets(fl + 1 + (fr - fl - 1 - 10) / 2, title, 10, C_QTITLE);
    } else {                                       /* input line */
        int f = (pw - 2 * DIALOG_PAD) - 10;

        setc(fl, 'x', C_QCD | DEC);
        setc(fr, 'x', C_QCD | DEC);
        sets(fl + 2, "cd", 2, C_QCD);
        line_view(&qline, f + 3, 3);
        line_draw(&qline, fl + 5, f, C_FIELD);
        sets(fl + 5 + f, "[^]", 3, C_HIST);
    }
}

/* ------------------------------------------------------------ error box */

/* Position of the error box: mc's wtools placement for message dialogs. */
static void err_geom(int *x, int *y)
{
    int h = ERR_H, w = ewidth;
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
    int dy = y - ey - DIALOG_PAD, fw = ewidth - 2 * DIALOG_PAD;
    int x0 = ex + DIALOG_PAD, r = x0 + fw - 1, x, i;

    if (y < ey || y >= ey + ERR_H) return;
    for (x = ex; x < ex + ewidth; x++) setc(x, ' ', C_ERR);
    if (dy < 0 || dy >= 7) return;
    if (dy == 0) {                                 /* top frame with title */
        setc(x0, 'l', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'k', C_ERR | DEC);
        sets(x0 + 2 + (fw - 4 - 7) / 2, " Error ", 7, C_ERRHEAD);
    } else if (dy == 6) {                          /* bottom frame */
        setc(x0, 'm', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'j', C_ERR | DEC);
    } else {                                       /* sides; the message lines, centred */
        setc(x0, 'x', C_ERR | DEC);
        setc(r, 'x', C_ERR | DEC);
        if (dy >= 2 && dy <= 4) {
            i = dy - 2;
            sets(x0 + 2 + (fw - 4 - tcols(eline[i], elen[i])) / 2, eline[i], elen[i], C_ERR);
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
enum { QY_ASK, QY_ERR, QY_DIR };
struct qy_button { const char *label; int hot; };   /* hot: the index of the hotkey in the label */
static const struct qy_button qy_yesno[2] = { { "[ Yes ]", 2 }, { "[ No ]", 2 } };
static const struct qy_button qy_recurse[5] = { { "[ Yes ]", 2 }, { "[ No ]", 2 }, { "[ All ]", 3 },
                                                 { "[ None ]", 5 }, { "[ Abort ]", 2 } };
static const struct qy_button qy_failed[4] = { { "[ Skip ]", 2 }, { "[ Skip all ]", 5 },
                                               { "[ Retry ]", 2 }, { "[ Abort ]", 2 } };
static int  qy_kind;
static char qy_t1[320], qy_t2[320];     /* the two lines of text */
static int  qy_n1, qy_n2;
static int  qy_sel;                     /* the selected button */
static int  qrx, qry, qrw, qrh;         /* the box, with its margin of one cell */

static const struct qy_button *qy_buttons(int *n)
{
    *n = qy_kind == QY_ASK ? 2 : qy_kind == QY_DIR ? 5 : 4;
    return qy_kind == QY_ASK ? qy_yesno : qy_kind == QY_DIR ? qy_recurse : qy_failed;
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
    qrw = m + 4 + 2 * DIALOG_PAD;
    qrh = 6 + 2 * DIALOG_PAD;          /* two lines of text, and the frame, with the margin */
    qrx = (cols - qrw + 1) / 2;
    qry = qy_kind != QY_ERR ? (rows - 6) / 3 : (rows - 4) / 2;
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
    int fx = qrx + DIALOG_PAD, fw = qrw - 2 * DIALOG_PAD, fh = 6, in = fw - 2;
    int dy = y - (qry + DIALOG_PAD), x, nb;
    const struct qy_button *b;

    if (y < qry || y >= qry + qrh) return;
    for (x = qrx; x < qrx + qrw; x++) setc(x, ' ', C_ERR);
    if (dy < 0 || dy >= fh) return;
    setc(fx, dy == 0 ? 'l' : dy == 5 ? 'm' : dy == 3 ? 't' : 'x', C_ERR | DEC);
    setc(fx + fw - 1, dy == 0 ? 'k' : dy == 5 ? 'j' : dy == 3 ? 'u' : 'x', C_ERR | DEC);
    if (dy == 0 || dy == 3 || dy == 5)
        for (x = fx + 1; x < fx + fw - 1; x++) setc(x, 'q', C_ERR | DEC);
    if (dy == 0) {
        const char *title = qy_kind != QY_ERR ? " Delete " : " Error ";
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

/* The job: F5, F6 or F8 done to a set of files. The set is the marked files and
 * directories of the panel if any is marked, else the highlighted item. Each item
 * is copied, moved or deleted in turn, and a directory is done as a whole: what is
 * in it is done too, recursively. A question is asked while the job runs, and the
 * job waits for its answer (modal_wait), so walking down a tree is plain recursion.
 * Esc, or Abort, stops the job at once: an item it has not finished keeps its mark. */
enum { JOB_COPY, JOB_MOVE, JOB_DEL };
static struct {
    int kind;                   /* JOB_COPY, JOB_MOVE or JOB_DEL */
    int sel;                    /* the marked items, not just the highlighted one */
    int dir;                    /* the destination is a directory: an item keeps its name in it */
    int pos;                    /* the next display index of the panel to look at */
    int skip_err;               /* Skip all was chosen: failures pass items over */
} job;
static char job_dst[4096];      /* the destination: a directory, or the file itself */
/* where the items are: the panel's directory, or the staging directory */
static char job_base[4096];
static long job_skips;          /* items passed over: a move keeps a source whose copy skipped any */

/* How a file is written, and what is decided about one that exists. */
enum { HOW_NEW, HOW_OVER, HOW_APPEND };
enum { DEC_NONE, DEC_NEW, DEC_OVER, DEC_APPEND, DEC_SKIP };

/* The overwrite question, as mc asks it. cf_group is the group answer that stands for
 * the rest of the job (CF_ASK: the question is asked for each file). The figures are
 * those of the pair in hand; cf_srcp and cf_dstp are its paths, as the question shows. */
enum { CF_ASK, CF_ALL, CF_OLDER, CF_NONE, CF_SMALLER, CF_SIZE };
#define CF_H 15                 /* rows of the question's box */
static int cf_group, cf_zero, cf_sel, cf_ans;
static uint64_t cf_ssize, cf_dsize;
static int64_t cf_smt, cf_dmt;
static const char *cf_srcp, *cf_dstp;

static int dgrp;                /* the answer to non-empty directories: 0 ask, 1 All, 2 None */
static int qy_ans;              /* the answer given to a failure or a directory question */

/* The input: the block read, from which the keys are taken, by the main loop and by
 * a question's wait alike. kbuf_eof is set when input has ended. */
static unsigned char kbuf[64];
static int kbuf_n, kbuf_i, kbuf_eof;

static int handle_key(int key);
static void modal_wait(void);
static int take_key(void);
static void job_run(void);

/* Whether two files are one file: the same device and inode. */
static int same_file(const struct statx *a, const struct statx *b)
{
    return a->stx_dev_major == b->stx_dev_major && a->stx_dev_minor == b->stx_dev_minor &&
           a->stx_ino == b->stx_ino;
}

/* The rule of the group answer for the pair in hand, as mc has it: a zero-length file
 * is not written over when the checkbox says so, except by Yes or Smaller. DEC_NONE
 * when the question is to be asked (CF_ASK). */
static int conflict_rule(void)
{
    int zero = cf_zero && cf_ssize == 0;

    switch (cf_group) {
    case CF_ALL:     return zero ? DEC_SKIP : DEC_OVER;
    case CF_OLDER:   return cf_smt > cf_dmt && !zero ? DEC_OVER : DEC_SKIP;
    case CF_NONE:    return DEC_SKIP;
    case CF_SMALLER: return cf_ssize > cf_dsize ? DEC_OVER : DEC_SKIP;
    case CF_SIZE:    return cf_ssize != cf_dsize && !zero ? DEC_OVER : DEC_SKIP;
    }
    return DEC_NONE;
}

/* The marked items: whether any is marked (a directory counts), and how many marked
 * files and directories there are. ".." is never marked. */
static int marked_any(const struct panel *p)
{
    for (int i = 0; i < p->ne; i++)
        if (p->e[i].sel && !p->e[i].up) return 1;
    return 0;
}

static void marked_counts(const struct panel *p, int *files, int *dirs)
{
    *files = *dirs = 0;
    for (int i = 0; i < p->ne; i++)
        if (p->e[i].sel && !p->e[i].up) {
            if (is_dir(&p->e[i])) (*dirs)++;
            else (*files)++;
        }
}

/* The display index of the next item the job works on, or -1 when there is none. */
static int job_next(const struct panel *p)
{
    for (; job.pos < p->n; job.pos++) {
        const struct ent *e = &p->e[p->ord[job.pos]];

        if (e->up) continue;
        if (job.sel ? e->sel : job.pos == p->cur) return job.pos++;
    }
    return -1;
}

/* F8 or ESC 8: the question. For marked items it counts them, worded as mc words it:
 * "files", "directories", or "files/directories" when both are there. For the
 * highlighted item it names it: "Delete directory" or "Delete file", and the name. */
static void delete_open(struct panel *p)
{
    const struct ent *e;
    char fitted[256];
    int n, files, dirs;

    if (p->arc[0]) {                                /* read-only: nothing is deleted */
        set_error("Read-only archive", p->arc, EROFS);
        return;
    }

    job.sel = marked_any(p);
    if (job.sel) {
        marked_counts(p, &files, &dirs);
        if (files + dirs == 0) return;
        qy_n1 = cat(qy_t1, "Delete ");
        if (files + dirs == 1) {
            qy_n1 += cat(qy_t1 + qy_n1, dirs ? "directory" : "file");
        } else {
            qy_n1 += (int)utoa((unsigned long)(files + dirs), qy_t1 + qy_n1);
            qy_n1 += cat(qy_t1 + qy_n1, dirs == 0 ? " files" : files == 0 ? " directories"
                                                              : " files/directories");
        }
        qy_n1 += cat(qy_t1 + qy_n1, "?");
        qy_n2 = 0;
    } else {
        if (p->n == 0) return;
        e = &p->e[p->ord[p->cur]];
        if (e->up) return;
        n = fit_name(fitted, p->nm + e->name, 43);
        qy_n1 = cat(qy_t1, is_dir(e) ? "Delete directory" : "Delete file");
        qy_n2 = 0;
        qy_t2[qy_n2++] = '"';
        for (int k = 0; k < n; k++) qy_t2[qy_n2++] = fitted[k];
        qy_t2[qy_n2++] = '"';
        qy_t2[qy_n2++] = '?';
    }
    qy_kind = QY_ASK;
    qy_sel = 0;
    mode = MODE_QUERY;
}

/* The choice of a button. Yes in the Delete question starts the job. Every other
 * question (a failure, a directory) only records its answer: the job waiting for it
 * goes on. */
static void qy_choose(int b)
{
    if (qy_kind == QY_ASK) {
        mode = MODE_NORMAL;
        if (b == 0) {
            job.kind = JOB_DEL;
            job.dir = 0;
            job_run();
        }
        return;
    }
    qy_ans = b;
    mode = MODE_NORMAL;
}

/* The keys of a question: its hotkeys, Enter for the selected button, Left and
 * Right (or Tab) to choose, and Esc, which is No or Abort. */
static int qy_handle(int key)
{
    int nb;
    const struct qy_button *b = qy_buttons(&nb);

    if (key == K_ESC) {
        qy_choose(qy_kind == QY_ASK ? 1 : nb - 1);
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

/* ------------------------------------------------ copy and move (F5, F6)
 *
 * A file is copied or moved in two steps, as mc does it: a question, with the
 * destination typed in a field (Enter does it, Esc gives up), and then a
 * progress box while the bytes go across. The destination starts as the other
 * panel's directory. */

static void draw(void);

enum { OP_COPY, OP_MOVE };
static char op_t1[320];                 /* the question's text: Copy "name" to: */
static int op_n1;
static struct line dline;               /* the destination, as typed */
static int opx, opy, opw, oph;          /* the box: column, row, width, height */
static const char *op_title[2] = { " Copy ", " Move " };
static const char *pg_src, *pg_dst;     /* the progress box: the source name, the destination */
static int pg_pct;                      /* percentage the progress box shows, -1 before the first */

/* The terminal size, if it has changed: the screen, the panels and the shell's
 * pseudo-terminal follow it. */
static void check_size(void)
{
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
}

/* S, N bytes of UTF-8, in W cells: all of it if it fits, else as much of its
 * end as fits after a ~ for what is cut from the front. */
static void show_tail(int x, const char *s, int n, int w, uint16_t a)
{
    int k = 0, total = tcols(s, n);

    if (total <= w) {
        sets(x, s, n, a);
        return;
    }
    while (total > w - 1 && k < n) total -= cwidth(utf8_get(s, n, &k));
    setc(x, '~', a);
    sets(x + 1, s + k, n - k, a);
}

/* The box: 64 cells wide, or less on a narrow screen, and in the upper third of
 * the screen. The question is six rows, the progress box nine. */
static void op_geom(void)
{
    opw = cols - 4 < 64 ? cols - 4 : 64;
    if (opw < 8) opw = 8;
    oph = (mode == MODE_PROG ? 9 : mode == MODE_CONFLICT ? CF_H : 6) + 2 * DIALOG_PAD;
    opx = (cols - opw) / 2;
    opy = (rows - oph) / 3;
    if (opx < 0) opx = 0;
    if (opy < 0) opy = 0;
}

/* The progress bar: [=====     ] and the percentage, in W cells at X. */
static void op_bar(int x, int w)
{
    int bw = w - 9, pct = pg_pct > 0 ? pg_pct : 0, filled = pct * bw / 100;
    char pc[4];

    if (bw < 1) return;
    setc(x, '[', C_QCD);
    for (int i = 0; i < bw; i++) setc(x + 1 + i, ' ', i < filled ? C_FIELD : C_QCD);
    setc(x + 1 + bw, ']', C_QCD);
    putnum(pc, (unsigned long)pct, 3);
    setc(x + 2 + bw, ' ', C_QCD);
    sets(x + 3 + bw, pc, 3, C_QCD);
    setc(x + 6 + bw, '%', C_QCD);
}

/* One row of the box.  The question: its text, the destination field, and the
 * buttons.  The progress box: source and destination, the bar and [ Abort ]. */
static void op_row(int dy)
{
    int prog = mode == MODE_PROG, fh = oph - 2 * DIALOG_PAD;
    int x0 = opx + DIALOG_PAD, r = opx + opw - 1 - DIALOG_PAD, in = OPFW - 4, x;
    int sep = prog ? 6 : 3, btn = prog ? 7 : 4;

    if (dy < 0 || dy >= oph) return;
    for (x = opx; x < opx + opw; x++) setc(x, ' ', C_QCD);     /* the margin too */
    dy -= DIALOG_PAD;
    if (dy < 0 || dy >= fh) return;
    if (dy == 0 || dy == fh - 1) {                       /* the frame, titled on top */
        setc(x0, dy ? 'm' : 'l', C_QCD | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        setc(r, dy ? 'j' : 'k', C_QCD | DEC);
        if (!dy) sets(x0 + (OPFW - 6) / 2, op_title[job.kind == JOB_COPY ? 0 : 1], 6, C_QTITLE);
        return;
    }
    setc(x0, dy == sep ? 't' : 'x', C_QCD | DEC);
    setc(r, dy == sep ? 'u' : 'x', C_QCD | DEC);
    if (dy == sep) {
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        return;
    }
    if (dy == btn) {
        if (prog) {
            qy_label(x0 + 2 + (in - 9) / 2, "[ Abort ]", 2, C_FIELD, C_QHOTSEL);
        } else {
            x = x0 + 2 + (in - 18) / 2;
            sets(x, "[ OK ]", 6, C_FIELD);
            sets(x + 8, "[ Cancel ]", 10, C_QCD);
        }
        return;
    }
    if (!prog) {
        if (dy == 1) sets(x0 + 2, op_t1, op_n1, C_QCD);
        if (dy == 2) {
            line_view(&dline, in, 0);
            line_draw(&dline, x0 + 2, in, C_FIELD);
        }
        return;
    }
    if (dy == 1) sets(x0 + 2, "Source", 6, C_QCD);
    if (dy == 2) show_tail(x0 + 2, pg_src, (int)strlen(pg_src), in, C_QCD);
    if (dy == 3) sets(x0 + 2, "Target", 6, C_QCD);
    if (dy == 4) show_tail(x0 + 2, pg_dst, (int)strlen(pg_dst), in, C_QCD);
    if (dy == 5) op_bar(x0 + 2, in);
}

/* Whether Esc, or A (the Abort of the progress box), has been typed alone, without
 * waiting for more input. Other keys typed during a copy are dropped. */
static int esc_typed(void)
{
    struct pollfd pf;
    unsigned char k[8];
    long n;

    pf.fd = 0;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0) return 0;
    n = read(0, k, sizeof k);
    if (n == 1 && (k[0] == 'a' || k[0] == 'A')) return 1;
    return n >= 1 && k[0] == K_ESC && (n == 1 || k[1] == K_ESC);
}

/* A chunk of the copy is done: the progress box is drawn again if its
 * percentage has moved. */
static void pg_update(uint64_t done, uint64_t total)
{
    int pct = total ? (int)udiv64(done * 100, total, NULL) : 100;

    if (pct > 100) pct = 100;
    if (pct == pg_pct) return;
    pg_pct = pct;
    check_size();
    draw();
}

/* Copy IN to OUT a chunk at a time, through ob: nothing else uses ob while the
 * copy runs, as draw() only runs once the chunk is written. Esc stops it, and
 * the result is -ECANCELED. */
static long copy_data(long in, long out, uint64_t total)
{
    uint64_t done = 0;

    for (;;) {
        long n = read((int)in, ob, sizeof ob), off = 0;

        if (n == -EINTR) continue;
        if (n <= 0) return n;
        while (off < n) {
            long w = write((int)out, ob + off, (size_t)(n - off));

            if (w == -EINTR) continue;
            if (w <= 0) return w < 0 ? w : -EIO;
            off += w;
        }
        done += (uint64_t)n;
        pg_update(done, total);
        if (esc_typed()) return -ECANCELED;
    }
}

/* Copy the file or link SRC to DST, made with the mode of SRC; a link is copied as
 * a link. HOW says what to do if DST exists: HOW_NEW (it must not), HOW_OVER
 * (truncate it), HOW_APPEND (add to its end). A copy that fails leaves no DST
 * behind, but only one made here: an existing file that was written over stays
 * as far as the copy got. Only files and links are copied. Returns 0, or -errno. */
static long copy_file(const char *src, const char *dst, int how)
{
    struct statx sx;
    char tg[4096];
    long in, out, r;
    uint32_t type;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx);
    if (r) return r;
    type = sx.stx_mode & S_IFMT;
    if (type == S_IFLNK) {
        r = readlinkat(AT_FDCWD, src, tg, sizeof tg - 1);
        if (r < 0) return r;
        tg[r] = 0;
        if (how != HOW_NEW) unlinkat(AT_FDCWD, dst, 0);    /* a link is replaced, not written in */
        return symlinkat(tg, AT_FDCWD, dst);
    }
    if (type != S_IFREG) return -EINVAL;
    in = open(src, O_RDONLY, 0);
    if (in < 0) return in;
    out = open(dst, O_WRONLY | O_CREAT |
               (how == HOW_NEW ? O_EXCL : how == HOW_OVER ? O_TRUNC : O_APPEND),
               sx.stx_mode & 07777);
    if (out < 0) {
        close((int)in);
        return out;
    }
    if (how != HOW_APPEND) fchmod((int)out, sx.stx_mode & 07777);   /* not left to the umask */
    r = copy_data(in, out, sx.stx_size);
    close((int)in);
    close((int)out);
    if (r && how == HOW_NEW) unlinkat(AT_FDCWD, dst, 0);
    return r;
}

/* Copy or move SRC to DST, with HOW as in copy_file(). A move is a rename, which
 * replaces DST; it cannot append, and across file systems it is a copy and then
 * the removal of SRC. A new DST must not exist. Returns 0, or -errno. */
static long op_file(const char *src, const char *dst, int how)
{
    struct statx sx;
    long r;

    if (how == HOW_NEW && statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0)
        return -EEXIST;
    if (job.kind == JOB_COPY) return copy_file(src, dst, how);
    if (how != HOW_APPEND) {
        r = renameat2(AT_FDCWD, src, AT_FDCWD, dst, 0);
        if (r != -EXDEV) return r;
    }
    r = copy_file(src, dst, how);
    if (r == 0) r = unlinkat(AT_FDCWD, src, 0);
    return r;
}

/* The overwrite question, as mc asks it: the new file and the one that exists,
 * with sizes and dates; Yes, No and Append for this file; and for the rest of the
 * job All, Older, None, Smaller and Size differs, with a checkbox for zero-length
 * files, and Abort. Items 0 to 9 are the buttons in focus order. */
static const char *const cf_label[10] = {
    "[ Yes ]", "[ No ]", "[ Append ]", "", "[ All ]", "[ Older ]", "[ None ]",
    "[ Smaller ]", "[ Size differs ]", "[ Abort ]"
};
static const char cf_hot[10] = { 'y', 'n', 'p', 'z', 'l', 'o', 'e', 'm', 's', 'a' };
static const unsigned char cf_at[10] = { 7, 7, 7, 10, 11, 11, 11, 11, 11, 13 };   /* each item's row */

static const char *cf_str(int i)
{
    if (i == 3) return cf_zero ? "[x] Don't overwrite with zero length file"
                               : "[ ] Don't overwrite with zero length file";
    return cf_label[i];
}

/* The index in S of its first letter HOT, either case, or -1: the hotkey's letter. */
static int hot_at(const char *s, int hot)
{
    for (int k = 0; s[k]; k++) {
        int c = (unsigned char)s[k];

        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == hot) return k;
    }
    return -1;
}

/* The buttons of row DY, centred in the IN cells after X0. */
static void cf_buttons(int dy, int x0, int in)
{
    int items[10], n = 0, w = 0, x;

    for (int i = 0; i < 10; i++)
        if (cf_at[i] == dy) items[n++] = i;
    for (int j = 0; j < n; j++) w += (int)strlen(cf_str(items[j])) + (j ? 1 : 0);
    x = x0 + 1 + (in - w) / 2;
    for (int j = 0; j < n; j++) {
        const char *s = cf_str(items[j]);
        int sel = items[j] == cf_sel;

        qy_label(x, s, hot_at(s, cf_hot[items[j]]), sel ? C_TITLE : C_ERR,
                 sel ? ATTR(BROWN, WHITE, 1) : C_ERRHEAD);
        x += (int)strlen(s) + 1;
    }
}

/* One row of the question. */
static void cf_row(int dy)
{
    int x0 = opx + DIALOG_PAD, r = opx + opw - 1 - DIALOG_PAD, x, in = OPFW - 2, l;
    char sz[16], tm[16];

    if (dy < 0 || dy >= oph) return;
    for (x = opx; x < opx + opw; x++) setc(x, ' ', C_ERR);     /* the margin too */
    dy -= DIALOG_PAD;
    if (dy < 0 || dy >= CF_H) return;
    if (dy == 0 || dy == CF_H - 1) {
        setc(x0, dy ? 'm' : 'l', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, dy ? 'j' : 'k', C_ERR | DEC);
        if (!dy) sets(x0 + (OPFW - 13) / 2, " File exists ", 13, C_ERRHEAD);
        return;
    }
    if (dy == 5 || dy == 8 || dy == 12) {
        setc(x0, 't', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'u', C_ERR | DEC);
        return;
    }
    setc(x0, 'x', C_ERR | DEC);
    setc(r, 'x', C_ERR | DEC);
    switch (dy) {
    case 1:
    case 3: {
        const char *p = dy == 1 ? cf_srcp : cf_dstp;

        sets(x0 + 2, dy == 1 ? "New     :" : "Existing:", 9, C_ERR);
        show_tail(x0 + 12, p, (int)strlen(p), r - x0 - 12, C_ERR);
        break;
    }
    case 2:
    case 4: {
        uint64_t size = dy == 2 ? cf_ssize : cf_dsize;
        int64_t mt = dy == 2 ? cf_smt : cf_dmt;

        l = fmt_size(sz, 7, size, 0);
        sets(x0 + 12, sz, l, C_ERR);
        fmt_date(tm, mt);
        sets(r - 13, tm, 12, C_ERR);
        break;
    }
    case 6:
        sets(x0 + 1 + (in - 20) / 2, "Overwrite this file?", 20, C_ERR);
        break;
    case 9:
        sets(x0 + 1 + (in - 20) / 2, "Overwrite all files?", 20, C_ERR);
        break;
    default:
        cf_buttons(dy, x0, in);
        break;
    }
}

/* The question's choice of button I. The checkbox just toggles; Abort ends the
 * job; the per-file answers decide this file; a group answer stands for the rest. */
static void cf_choose(int i)
{
    cf_sel = i;
    if (i == 3) {                       /* the checkbox just toggles */
        cf_zero ^= 1;
        return;
    }
    cf_ans = i;
    mode = MODE_NORMAL;
}

/* The keys of the question: Enter for the focused button, Left and Right (and
 * Tab) to move the focus, the hotkeys, and Esc for Abort. */
static int cf_handle(int key)
{
    if (key == K_ESC) {
        cf_choose(9);
        return 0;
    }
    if (key == '\r' || key == '\n') {
        cf_choose(cf_sel);
        return 0;
    }
    if (key == K_LEFT) {
        cf_sel = (cf_sel + 9) % 10;
        return 0;
    }
    if (key == K_RIGHT || key == 9) {
        cf_sel = (cf_sel + 1) % 10;
        return 0;
    }
    if (key >= 'A' && key <= 'Z') key += 32;
    for (int i = 0; i < 10; i++)
        if (key == cf_hot[i]) {
            cf_choose(i);
            return 0;
        }
    return 0;
}

/* the walk is recursive: these call each other */
static long del_tree(const char *path, int approved);
static long copy_tree(const char *src, const char *dst);
static long move_tree(const char *src, const char *dst);

/* A failure: the box says what failed, and why, and offers Skip, Skip all, Retry and
 * Abort. Returns 0 to pass the item over, 1 to try it again, or -ECANCELED. */
static long fail_ask(const char *head, const char *path, long e)
{
    int k = 0;

    if (job.skip_err) {
        job_skips++;
        return 0;
    }
    qy_kind = QY_ERR;
    qy_n1 = cat(qy_t1, head);
    qy_t1[qy_n1++] = ' ';
    qy_t1[qy_n1++] = '"';
    while (path[k] && qy_n1 < (int)sizeof qy_t1 - 3) qy_t1[qy_n1++] = path[k++];
    qy_t1[qy_n1++] = '"';
    if (e == -ESAME) {
        qy_n2 = cat(qy_t2, "Same file as the source");
    } else if (e == -ESUB) {
        qy_n2 = cat(qy_t2, "Inside the directory itself");
    } else {
        qy_n2 = cat(qy_t2, errstr(-e));
        qy_t2[qy_n2++] = ' ';
        qy_t2[qy_n2++] = '(';
        qy_n2 += (int)utoa((unsigned long)-e, qy_t2 + qy_n2);
        qy_t2[qy_n2++] = ')';
    }
    qy_sel = 0;
    mode = MODE_QUERY;
    modal_wait();
    if (kbuf_eof) return -ECANCELED;
    if (qy_ans == 1) job.skip_err = 1;
    if (qy_ans == 0 || qy_ans == 1) {
        job_skips++;
        return 0;
    }
    if (qy_ans == 2) return 1;
    return -ECANCELED;
}

/* An item that cannot be tried again: the box, and its answer. Retry counts as Skip. */
static long item_fail(const char *head, const char *path, long e)
{
    long a = fail_ask(head, path, e);

    if (a < 0) return a;
    if (a == 1) job_skips++;
    return 0;
}

/* The question for a destination that exists: 0 to write it as *how says, 1 to pass
 * the file over, -ESAME for the same file, or -ECANCELED. */
static long conflict(const char *src, const char *dst, int *how)
{
    struct statx ss, ds;
    int dec;

    *how = HOW_NEW;
    if (statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds)) return 0;
    if (statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss)) return 0;
    if (same_file(&ss, &ds)) return -ESAME;
    cf_ssize = ss.stx_size;
    cf_smt = ss.stx_mtime.tv_sec;
    cf_dsize = ds.stx_size;
    cf_dmt = ds.stx_mtime.tv_sec;
    cf_srcp = src;
    cf_dstp = dst;
    if (cf_group == CF_ASK) {
        cf_ans = -1;
        cf_sel = 0;
        mode = MODE_CONFLICT;
        modal_wait();
        if (kbuf_eof || cf_ans < 0 || cf_ans == 9) return -ECANCELED;
        switch (cf_ans) {
        case 0: dec = DEC_OVER; break;
        case 1: dec = DEC_SKIP; break;
        case 2: dec = DEC_APPEND; break;
        default:                            /* a group answer stands for the rest */
            cf_group = cf_ans == 4 ? CF_ALL : cf_ans == 5 ? CF_OLDER : cf_ans == 6 ? CF_NONE
                     : cf_ans == 7 ? CF_SMALLER : CF_SIZE;
            dec = conflict_rule();
        }
    } else {
        dec = conflict_rule();
    }
    if (dec == DEC_SKIP) {
        job_skips++;
        return 1;
    }
    *how = dec == DEC_OVER ? HOW_OVER : dec == DEC_APPEND ? HOW_APPEND : HOW_NEW;
    return 0;
}

/* A non-empty directory at the top of the job: is it deleted with what is in it? The
 * answer stands for the directories after it, as All and None say. Returns 1 to
 * delete it, 0 to skip it, or -ECANCELED. */
static long dir_ask(const char *path)
{
    int k = 0;

    if (dgrp) return dgrp == 1;
    qy_kind = QY_DIR;
    qy_n1 = cat(qy_t1, "Directory \"");
    while (path[k] && qy_n1 < (int)sizeof qy_t1 - 16) qy_t1[qy_n1++] = path[k++];
    qy_n1 += cat(qy_t1 + qy_n1, "\" not empty.");
    qy_n2 = cat(qy_t2, "Delete it recursively?");
    qy_sel = 0;
    mode = MODE_QUERY;
    modal_wait();
    if (kbuf_eof) return -ECANCELED;
    switch (qy_ans) {
    case 0: return 1;
    case 1: return 0;
    case 2: dgrp = 1; return 1;
    case 3: dgrp = 2; return 0;
    default: return -ECANCELED;
    }
}

/* DIR and NAME into D (max bytes): a path. Returns 0, or -ENAMETOOLONG. */
static long join(char *d, size_t max, const char *dir, const char *name)
{
    size_t ld = strlen(dir), ln = strlen(name), k;
    int slash = ld && dir[ld - 1] != '/';

    if (ld + (slash ? 1 : 0) + ln + 1 > max) return -ENAMETOOLONG;
    memcpy(d, dir, ld);
    k = ld;
    if (slash) d[k++] = '/';
    memcpy(d + k, name, ln);
    d[k + ln] = 0;
    return 0;
}

/* Whether D lies inside the directory S. */
static int under(const char *d, const char *s)
{
    size_t l = strlen(s), k;

    if (strlen(d) <= l || d[l] != '/') return 0;
    for (k = 0; k < l; k++)
        if (d[k] != s[k]) return 0;
    return 1;
}

/* Calls FN with each name in the directory PATH, but "." and "..", until FN returns
 * non-zero, which is what this returns; or the -errno of the directory, if it cannot
 * be read. */
static long each_entry(const char *path, long (*fn)(const char *name, void *ctx), void *ctx)
{
    long buf[128];                      /* 1 KiB, aligned for the records */
    long fd, n = 0, off, r = 0;

    fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return fd;
    while (!r && (n = getdents64((int)fd, buf, sizeof buf)) > 0) {
        for (off = 0; off < n && !r; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + off);
            const char *nm = d->d_name;

            off += d->d_reclen;
            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
            r = fn(nm, ctx);
        }
    }
    if (n < 0 && !r) r = n;
    close((int)fd);
    return r;
}

/* The directory's entries, one at a time, deleted. */
static long del_child(const char *name, void *ctx)
{
    const char *dir = ctx;
    char child[4352];
    long r = join(child, sizeof child, dir, name);

    if (r) return item_fail("Cannot remove", dir, r);
    return del_tree(child, 1);
}

/* Deletes PATH: a file or a link is unlinked; a directory is removed when empty.
 * When it is not, it is deleted with what is in it, if that is approved: a top item
 * is asked about (dir_ask), and the items under it are approved with it. Returns 0,
 * or -ECANCELED. */
static long del_tree(const char *path, int approved)
{
    struct statx sx;
    long r, a;

    for (;;) {
        if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx)) return 0;
        if ((sx.stx_mode & S_IFMT) != S_IFDIR) {
            r = unlinkat(AT_FDCWD, path, 0);
            if (r == 0) return 0;
            a = fail_ask("Cannot remove file", path, r);
        } else {
            r = unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
            if (r == 0) return 0;
            if (r == -ENOTEMPTY || r == -EEXIST) {
                if (!approved) {
                    a = dir_ask(path);
                    if (a <= 0) {
                        if (a == 0) job_skips++;
                        return a;
                    }
                    approved = 1;
                }
                r = each_entry(path, del_child, (void *)path);
                if (r == -ECANCELED) return r;
                if (r == 0) {
                    r = unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
                    if (r == 0) return 0;
                }
            }
            a = fail_ask("Cannot remove directory", path, r);
        }
        if (a < 0) return a;
        if (a == 0) return 0;
    }
}

struct tctx { const char *src, *dst; };

/* One entry of a directory being copied or moved: its source and destination. */
static long copy_child(const char *name, void *ctx)
{
    const struct tctx *t = ctx;
    char cs[4352], cd[4352];
    long r = join(cs, sizeof cs, t->src, name);

    if (!r) r = join(cd, sizeof cd, t->dst, name);
    if (r) return item_fail("Cannot copy to", t->dst, r);
    return copy_tree(cs, cd);
}

static long move_child(const char *name, void *ctx)
{
    const struct tctx *t = ctx;
    char cs[4352], cd[4352];
    long r = join(cs, sizeof cs, t->src, name);

    if (!r) r = join(cd, sizeof cd, t->dst, name);
    if (r) return item_fail("Cannot move to", t->dst, r);
    return move_tree(cs, cd);
}

/* A directory is made at DST with MODE: the umask has been at it, so the mode is set
 * after. Returns 0, 1 to pass the directory over, or -ECANCELED. */
static long make_dir(const char *dst, int mode)
{
    long r, a, fd;

    for (;;) {
        r = mkdirat(AT_FDCWD, dst, 0777);
        if (r == 0) break;
        if (r == -EEXIST) return 0;
        a = fail_ask("Cannot create directory", dst, r);
        if (a < 0) return a;
        if (a == 0) return 1;
    }
    fd = open(dst, O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        fchmod((int)fd, mode);
        close((int)fd);
    }
    return 0;
}

/* One file, link or directory-less item of a copy or move, as the question has it:
 * the destination is asked about when it exists, and a failure of the write is asked
 * about. Returns 0, or -ECANCELED. */
static long leaf(const char *src, const char *dst, int move)
{
    int how;
    long r, a;

    r = conflict(src, dst, &how);
    if (r == -ESAME) return item_fail(move ? "Cannot move to" : "Cannot copy to", dst, r);
    if (r == 1) return 0;
    if (r) return r;
    for (;;) {
        pg_src = src;
        pg_dst = dst;
        pg_pct = -1;
        mode = MODE_PROG;
        r = move ? op_file(src, dst, how) : copy_file(src, dst, how);
        if (r == 0 || r == -ECANCELED) return r;
        a = fail_ask(move ? "Cannot move to" : "Cannot copy to", dst, r);
        if (a < 0) return a;
        if (a == 0) return 0;
    }
}

/* A copy of SRC to DST: a file, a link, or a directory made, and what is in it copied
 * too; a directory that is there already is merged into. Returns 0, or -ECANCELED. */
static long copy_tree(const char *src, const char *dst)
{
    struct statx ss, ds;
    struct tctx t;
    long r;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss);
    if (r) return item_fail("Cannot copy", src, r);
    if ((ss.stx_mode & S_IFMT) != S_IFDIR) return leaf(src, dst, 0);
    if (under(dst, src)) return item_fail("Cannot copy to", dst, -ESUB);
    r = statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds);
    if (r == 0) {
        if ((ds.stx_mode & S_IFMT) != S_IFDIR) return item_fail("Cannot copy to", dst, -ENOTDIR);
        if (same_file(&ss, &ds)) return item_fail("Cannot copy to", dst, -ESAME);
    } else {
        r = make_dir(dst, ss.stx_mode & 07777);
        if (r) return r < 0 ? r : 0;
    }
    t.src = src;
    t.dst = dst;
    r = each_entry(src, copy_child, &t);
    if (r == -ECANCELED) return r;
    if (r) return item_fail("Cannot copy", src, r);
    return 0;
}

/* A move of SRC to DST: a rename when DST is free; across file systems, a copy and
 * then the removal of SRC, which is kept if anything in it was passed over. Into a
 * directory that is there, the move is into it, item by item, and the directory
 * goes when it is empty. Returns 0, or -ECANCELED. */
static long move_tree(const char *src, const char *dst)
{
    struct statx ss, ds;
    struct tctx t;
    long r, s0;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss);
    if (r) return item_fail("Cannot move", src, r);
    if ((ss.stx_mode & S_IFMT) != S_IFDIR) return leaf(src, dst, 1);
    if (under(dst, src)) return item_fail("Cannot move to", dst, -ESUB);
    r = statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds);
    if (r == -ENOENT) {
        r = renameat2(AT_FDCWD, src, AT_FDCWD, dst, 0);
        if (r == 0) return 0;
        if (r != -EXDEV) return item_fail("Cannot move to", dst, r);
        s0 = job_skips;
        r = copy_tree(src, dst);
        if (r) return r;
        if (job_skips != s0) return 0;
        return del_tree(src, 1);
    }
    if (r) return item_fail("Cannot move to", dst, r);
    if ((ds.stx_mode & S_IFMT) != S_IFDIR) return item_fail("Cannot move to", dst, -ENOTDIR);
    if (same_file(&ss, &ds)) return item_fail("Cannot move to", dst, -ESAME);
    t.src = src;
    t.dst = dst;
    s0 = job_skips;
    r = each_entry(src, move_child, &t);
    if (r == -ECANCELED) return r;
    if (r) return item_fail("Cannot move", src, r);
    if (job_skips != s0) return 0;
    r = unlinkat(AT_FDCWD, src, AT_REMOVEDIR);
    if (r == 0) return 0;
    return item_fail("Cannot remove directory", src, r);
}

/* One item of the job, at the top: SRC, and DST when copying or moving. */
static long job_item(const char *src, const char *dst)
{
    if (job.kind == JOB_DEL) return del_tree(src, 0);
    if (job.kind == JOB_COPY) return copy_tree(src, dst);
    return move_tree(src, dst);
}

/* The job is over: the panels read their files again. */
/* F3 on a file of the archive panel P: the file is written to a staging directory of
 * its own, mcview runs on it, and the directory goes when mcview has exited. */
static void vfs_view(struct panel *p, const struct ent *e)
{
    char inner[512], path[4352], *argv[3];
    struct statx sx;
    long r;

    strlcpy(inner, p->sub, sizeof inner);
    strlcat(inner, p->nm + e->name, sizeof inner);
    xo.n = 0;
    xo.used = 0;
    xo.err = 0;
    r = xtmp_make();
    if (!r) r = xo_add(inner);
    if (!r) {
        vfs_hard_targets(p);
        r = archive_scan(p, NULL, &xo);
    }
    if (!r && join(path, sizeof path, xo.root, inner)) r = -ENAMETOOLONG;
    if (!r && (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) != 0 ||
               (sx.stx_mode & S_IFMT) != S_IFREG))
        r = -ENOENT;
    if (r) {
        set_error("Cannot extract", inner, -r);
    } else {
        argv[0] = (char *)"mcview";
        argv[1] = path;
        argv[2] = NULL;
        run_child("mcview", argv, xo.root, RUN_SEARCH, NULL);
    }
    if (xo.root[0]) del_tree(xo.root, 1);
    xo.root[0] = 0;
}

static void job_end(void)
{
    mode = MODE_NORMAL;
    rescan_changed();
}

/* The items, in display order, one after another. An item that Esc stopped keeps its
 * mark; every other item loses it, done or passed over. */
static void job_run(void)
{
    struct panel *p = &P[act];
    char src[4352], dst[4352];

    job.pos = 0;
    job.skip_err = 0;
    dgrp = 0;
    cf_group = CF_ASK;
    cf_zero = 0;
    cf_sel = 0;
    while (!kbuf_eof) {
        int at = job_next(p);
        const struct ent *e;
        long r;

        if (at < 0) break;
        e = &p->e[p->ord[at]];
        r = join(src, sizeof src, job_base, p->nm + e->name);
        if (!r) {
            if (job.dir) r = join(dst, sizeof dst, job_dst, p->nm + e->name);
            else r = strlcpy(dst, job_dst, sizeof dst) < sizeof dst ? 0 : -ENAMETOOLONG;
        }
        if (r) {
            if (item_fail("Cannot copy to", job_dst, r) == -ECANCELED) break;
            p->e[p->ord[at]].sel = 0;
            continue;
        }
        r = job_item(src, dst);
        if (r == -ECANCELED) break;
        p->e[p->ord[at]].sel = 0;
    }
    job_end();
}

/* The keys of a question of the job: handled as usual, until the question is answered
 * (its handler leaves the question's mode). Called from inside the handler of the key
 * that started the job, reading from the block the main loop reads from. */
static void modal_wait(void)
{
    while (!kbuf_eof && (mode == MODE_QUERY || mode == MODE_CONFLICT)) {
        int key;

        draw();
        key = take_key();
        if (key != K_NONE) handle_key(key);
    }
}

/* The last key taken was an Esc that another Esc followed at once. Each Esc after
 * such an Esc is taken at once too, as long as it comes within ESC_WAIT_MS of the one
 * before: it is a key of its own, not the start of a sequence (no sequence begins
 * with Esc Esc). esc_ts is the time of the last Esc of the chain. */
static int esc_chain;
static struct timespec esc_ts;      /* when the last Esc of the chain was taken */

/* The time of the last Esc of the chain: esc_ts. */
static void esc_mark(void)
{
    clock_gettime(CLOCK_REALTIME, &esc_ts);
}

/* Whether less than ESC_WAIT_MS has passed since esc_mark(). No 64-bit arithmetic, and
 * no division, which some targets would make a library call: the difference is taken
 * in seconds and nanoseconds, and only up to one second's difference is measured, so
 * it fits 32 bits. */
static int esc_recent(void)
{
    struct timespec ts;
    long ds, el;

    clock_gettime(CLOCK_REALTIME, &ts);
    ds = ts.tv_sec - esc_ts.tv_sec;
    if (ds < 0 || ds > 1) return 0;
    el = ds * 1000000000L + (ts.tv_nsec - esc_ts.tv_nsec);
    return el >= 0 && el < ESC_WAIT_MS * 1000000L;
}

/* The next key of the input block. A lone Esc at the end of the block may be the start
 * of a sequence, so it waits ESC_WAIT_MS for the rest, unless it follows an Esc taken
 * at once (above). The screen is drawn before such a wait, so what has been done shows
 * while the rest may come. When the block is used up it is read again. K_NONE: nothing
 * to handle here (a sequence cut off is dropped, or a read was interrupted). */
static int take_key(void)
{
    int len, key;

    for (;;) {
        if (kbuf_i >= kbuf_n) {
            long n = read(0, kbuf, sizeof kbuf);

            if (n == -EINTR) {
                check_size();
                return K_NONE;
            }
            if (n <= 0) {
                kbuf_eof = 1;
                return K_NONE;
            }
            kbuf_n = (int)n;
            kbuf_i = 0;
        }
        key = key_parse(kbuf, kbuf_n, kbuf_i, &len);
        if (key != K_INCOMPLETE) {
            kbuf_i += len;
            esc_chain = key == K_ESC;
            if (esc_chain) esc_mark();
            return key;
        }
        if (esc_chain && kbuf[kbuf_i] == K_ESC) {
            if (esc_recent()) {                      /* an Esc after an Esc: taken at once */
                kbuf_i++;
                esc_mark();
                return K_ESC;
            }
            esc_chain = 0;                           /* too long after the last: a new key */
        }
        draw();
        {
            int m = wait_input(kbuf, kbuf_n, sizeof kbuf, ESC_WAIT_MS);

            if (m > kbuf_n) {
                kbuf_n = m;
                continue;
            }
            esc_chain = 0;
            if (kbuf[kbuf_i] == K_ESC && kbuf_i + 1 == kbuf_n) {
                kbuf_i++;
                return K_ESC;
            }
            kbuf_i = kbuf_n;
            return K_NONE;
        }
    }
}

/* Enter in the question: the job starts. A destination that is a directory takes
 * each item under its own name; marked items need a directory to go to. An empty
 * destination does nothing. */
static void op_confirm(void)
{
    struct panel *p = &P[act];
    char res[4096], shown[512];
    struct statx sx;
    int isdir;

    if (!dline.len) {
        mode = MODE_NORMAL;
        return;
    }
    path_of(dline.buf, p->path, res, sizeof res, shown, sizeof shown);
    isdir = statx(AT_FDCWD, res, 0, STATX_BASIC_STATS, &sx) == 0 &&
            (sx.stx_mode & S_IFMT) == S_IFDIR;
    if (job.sel && !isdir) {
        mode = MODE_NORMAL;
        set_error(job.kind == JOB_COPY ? "Cannot copy to" : "Cannot move to", res, ENOTDIR);
        return;
    }
    strlcpy(job_dst, res, sizeof job_dst);
    job.dir = isdir;
    if (p->arc[0]) {
        long r = vfs_stage(p, job.sel);

        if (r) {
            if (xo.root[0]) del_tree(xo.root, 1);
            xo.root[0] = 0;
            mode = MODE_NORMAL;
            set_error("Cannot extract from", p->arc, -r);
            return;
        }
        strlcpy(job_base, xo.root, sizeof job_base);
        job_run();
        del_tree(xo.root, 1);
        xo.root[0] = 0;
        return;
    }
    strlcpy(job_base, p->path, sizeof job_base);
    job_run();
}

/* F5 or F6: the question. For marked items it gives their number, else it names the
 * highlighted item, as a file or a directory. The destination starts as the other
 * panel's directory. Nothing is asked when nothing is marked and the highlight is on
 * "..". */
static void op_open(int kind)
{
    struct panel *p = &P[act];
    const struct ent *e = NULL;
    const char *name = "";
    int n, k, l, files, dirs;

    if (kind == JOB_MOVE && p->arc[0]) {
        set_error("Read-only archive", p->arc, EROFS);
        return;
    }
    if (P[act ^ 1].arc[0]) {                        /* nothing is written into an archive */
        set_error("Read-only archive", P[act ^ 1].arc, EROFS);
        return;
    }

    job.kind = kind;
    job.sel = marked_any(p);
    if (job.sel) {
        marked_counts(p, &files, &dirs);
        if (files + dirs == 0) return;
    } else {
        if (!p->n) return;
        e = &p->e[p->ord[p->cur]];
        if (e->up) return;
        name = p->nm + e->name;
    }
    mode = MODE_OP;
    op_geom();
    n = cat(op_t1, kind == JOB_COPY ? "Copy " : "Move ");
    if (job.sel) {
        n += (int)utoa((unsigned long)(files + dirs), op_t1 + n);
        n += cat(op_t1 + n, files + dirs == 1 ? " file to:" : " files to:");
    } else {
        if (is_dir(e)) n += cat(op_t1 + n, "directory ");
        n += cat(op_t1 + n, "\"");
        k = cut_cols(name, (int)strlen(name), opw - 26);
        for (int i = 0; i < k; i++) op_t1[n++] = name[i];
        n += cat(op_t1 + n, "\" to:");
    }
    op_n1 = n;
    line_clear(&dline);
    l = (int)strlen(P[act ^ 1].path);
    if (l + 2 < (int)sizeof dline.buf) {
        cat(dline.buf, P[act ^ 1].path);
        if (dline.buf[l - 1] != '/') dline.buf[l++] = '/';
        dline.buf[l] = 0;
        dline.len = dline.pos = l;
    }
}

/* Keys in the question: Enter does it, Esc gives up, the rest edits the
 * destination. */
static int op_handle(int key)
{
    if (key == K_ESC) mode = MODE_NORMAL;
    else if (key == '\r' || key == '\n') op_confirm();
    else line_key(&dline, key, OPFW - 4, 0);
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
    if (mode == MODE_OP || mode == MODE_PROG || mode == MODE_CONFLICT) op_geom();
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
        if (mode == MODE_QCD) qcd_draw_row(y, ax, aw);
        if (mode == MODE_ERR) err_draw_row(y, ex, ey);
        if (mode == MODE_QUERY) qy_draw_row(y);
        if (mode == MODE_OP || mode == MODE_PROG || mode == MODE_CONFLICT) {
            if (mode == MODE_CONFLICT) cf_row(y - opy);
            else op_row(y - opy);
            if (y >= opy + 1 && y <= opy + oph) shade(opx + opw, opx + opw + 1);
            if (y == opy + oph) shade(opx + 2, opx + opw + 1);
        }
        if (mode == MODE_QUERY) {
            if (y >= qry + 1 && y <= qry + qrh) shade(qrx + qrw, qrx + qrw + 1);
            if (y == qry + qrh) shade(qrx + 2, qrx + qrw + 1);
        }
        /* shadows: to the right of each box, and under it */
        if (mode == MODE_QCD) {
            int top = rows - 2 - QCD_H;

            if (y >= top + 1 && y <= top + QCD_H - 1) shade(ax + aw, ax + aw + 1);
            if (y == top + QCD_H) shade(ax + 2, ax + aw + 1);
        }
        if (mode == MODE_ERR) {
            if (y >= ey + 1 && y <= ey + ERR_H) shade(ex + ewidth, ex + ewidth + 1);
            if (y == ey + ERR_H) shade(ex + 2, ex + ewidth + 1);
        }
        emit_row(y, cols);
    }

    /* cursor: in the quick cd field, otherwise on the command line */
    if (mode == MODE_QCD) {
        cy = rows - 4 - DIALOG_PAD;
        cx = ax + DIALOG_PAD + 5 + line_cells(&qline);
    } else if (mode == MODE_OP) {
        cy = opy + DIALOG_PAD + 2;
        cx = opx + DIALOG_PAD + 2 + line_cells(&dline);
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
    if (key == 8 || key == 127) {                 /* a character goes, whole */
        if (qline.len) qline.len = char_prev(&qline, qline.len);
        qline.buf[qline.len] = 0;
        return 0;
    }
    if ((key >= 0x20 && key < 0x7f) || (key & K_CHAR)) {
        char b[4];
        int n, at;

        if (key & K_CHAR) {
            n = utf8_enc(b, (uint32_t)(key & ~K_CHAR));
        } else {
            b[0] = (char)key;
            n = 1;
        }
        if (qline.len + n >= (int)sizeof qline.buf) return 0;
        memcpy(qline.buf + qline.len, b, n);
        at = qs_find(p, p->cur, qline.buf, qline.len + n);
        if (at >= 0) {
            qline.len += n;
            qline.buf[qline.len] = 0;
            p->cur = at;
            scroll(p);
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
    if (mode == MODE_CONFLICT) return cf_handle(key);
    if (mode == MODE_OP) return op_handle(key);
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
    /* Alt+Enter: the highlighted name goes on the command line (put_name) */
    if (key == (K_ALT | '\r') || key == (K_ALT | '\n')) {
        put_name(p);
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
    else if (key == K_F5 || key == (K_ALT | '5')) op_open(OP_COPY);
    else if (key == K_F6 || key == (K_ALT | '6')) op_open(OP_MOVE);
    else if (key == 21) lft ^= 1;                  /* Ctrl+U */
    else if (key == K_INS || key == K_SUP || key == K_SDOWN) sel_key(p, key);
    else if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN ||
             key == K_HOME || key == K_END)
        nav_key(p, key);
    else if (key == K_LEFT || key == K_RIGHT || key == K_CLEFT || key == K_CRIGHT ||
             key == K_ALTBS || key == K_DEL || key == 8 || key == 127 ||
             (key >= 0x20 && key < 0x7f) || (key >= 0x80 && key < 0x100) || (key & K_CHAR))
        line_key(&cmdl, key, cmd_width(), 0);
    return 0;
}

__attribute__((noreturn, noinline)) static void main(int argc, char **argv)
{
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
        check_size();
        draw();

        if (!wait_term()) continue;          /* SIGWINCH: the loop re-reads the size */
        n = read(0, kbuf, sizeof kbuf);
        if (n == -EINTR) continue;
        if (n <= 0) break;
        kbuf_n = (int)n;
        kbuf_i = 0;

        while (kbuf_i < kbuf_n && !quit) {
            int key;

            if (hidden) {                    /* the shell view: as typed, but Ctrl+O */
                int j = kbuf_i;

                while (j < kbuf_n && kbuf[j] != 15) j++;
                shell_in(kbuf + kbuf_i, j - kbuf_i);
                if (j == kbuf_n) {
                    kbuf_i = kbuf_n;
                    break;
                }
                kbuf_i = j + 1;
                toggle_hidden();
                continue;
            }
            key = take_key();
            if (key == K_NONE) continue;
            quit = handle_key(key);
        }
    }

    tty_leave();
    exit(0);
    __builtin_unreachable();
}
