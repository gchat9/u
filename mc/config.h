/* config.h - mc's parameters: sizes, colours and file classes. Change a value
 * here, then make. */

/* Sizes and time */
#define MAXE   4096          /* entries per directory; extras are dropped */
#define NPOOL  131072        /* bytes of names per panel; extras are dropped */
#define MAXW   1024          /* widest terminal we draw */
#define TZMAX  8192          /* room for /etc/localtime */
#define AR_MAXE  (1u << 20)             /* entries kept per archive; the rest are dropped */
#define AR_NPOOL (1u << 26)             /* bytes of names kept per archive; the rest are dropped */
#define ESC_WAIT_MS 1000        /* after an ESC, how long the rest of a key may take */

/* Look: 1 makes directories bold white, 0 bright white. DIALOG_PAD is the
 * margin of every dialog, in cells, round its frame; 0 for flush with the edge. */
#define DIR_BOLD   0
#define DIALOG_PAD 1

/* Colours: the ANSI colour numbers, then mc's skin (ATTR is in mc.c) */
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
#define C_SEL    ATTR(BROWN, BLUE, 1)   /* a marked item */
#define C_SELCUR ATTR(BROWN, CYAN, 1)   /* a marked item under the cursor */
#define C_INFO   ATTR(WHITE, BLUE, 0)
#define C_FREE   ATTR(WHITE, BLUE, 0)

#if DIR_BOLD
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
#define C_QHOT   ATTR(BLUE, WHITE, 1)   /* a hotkey letter, white dialog */

#define C_QHOTSEL ATTR(BLUE, CYAN, 1)   /* a hotkey letter, on the focused button */

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
      "tar;tbz;tbz2;tgz;tlz;txz;tzst;vsix;xz;zip;zoo;zst;zstd", CL_ARCHIVE },
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
