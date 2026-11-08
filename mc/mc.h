/* mc.h - what the parts of mc share: its types, its state, and the few functions
 * that are called before they are defined. */

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

static int64_t ent_mtime(const struct ent *e) { return (int64_t)e->mtime; }

struct archive;

/* The kinds of archive, by their names (archive_kind). */
enum { AR_NONE, AR_ZIP, AR_TAR, AR_GZ, AR_XZ, AR_ZST };

/* The sort of a panel: its field, and flags, in one byte.  Zero is the default:
 * by name, case sensitive, directories first. */
enum { SF_NAME, SF_UNSORTED, SF_VERSION, SF_EXT, SF_SIZE, SF_MTIME, SF_ATIME, SF_CTIME, SF_INODE };
#define SO_FIELD 0x0f       /* the SF_ field */
#define SO_FOLD  0x10       /* names compare without case */
#define SO_REV   0x20       /* in reverse */
#define SO_EXEC  0x40       /* executables after the directories, before the rest */

struct panel {
    char     path[4096];         /* the directory; inside an archive, the one holding it */
    char     arc[256];           /* the archive shown as a directory, "" if none */
    char     sub[256];           /* the directory inside the archive: "a/b/", "" at its root */
    struct archive *ar;          /* the archive's index, while arc is set */
    int      n, cur, top;        /* entries shown, cursor, first visible entry */
    int      ne;                 /* entries read, including e[0] = ".." */
    uint8_t  sort;               /* SF_ and SO_ bits, see sort_list() */
    uint8_t  rem;                /* 1: on a shell link, its path is the directory there */
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

static struct panel P[2];
static struct cell  row[MAXW];
static int rfull = 1;            /* repaint everything: the terminal shows something else now */
static int rlast = -1;           /* the mode of the last draw */
static char         ob[65536];   /* output buffer; a file is copied through it, a chunk at a time */
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
       MODE_CONFLICT, MODE_MENU, MODE_SORT, MODE_LINK, MODE_GROUP };
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

/* The shell link state (see shell links): user@host of the link, the local directory
 * it was opened from, and ssh's process and pipes. */
static char rhost[256];                 /* user@host of the link, "" if none */
static char rorig[4096];                /* the local directory the link was opened from */
static long rpid = -1;                  /* ssh, -1 if not running */
static int  rw = -1, rr = -1;           /* ssh's input and output */
static char rb[1024];                   /* read from ssh, not yet taken */
static int  rn, rpos;

/* Called before they are defined: the main loop's key handler, the screen redraw
 * and the check of the terminal's size. */
static int handle_key(int key);
static void draw(void);
static void check_size(void);
