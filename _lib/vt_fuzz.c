/*
 * vt_fuzz.c -- drives vt_feed() with the traffic a hostile server can send, for
 * vt_test.py (built with ASan/UBSan).  Usage:
 *   vt_fuzz repro            the screen shrink that left the saved cursor out of range
 *   vt_fuzz dec              DEC special graphics (ESC ( 0) and its reset (ESC ( B, RIS)
 *   vt_fuzz bell             BEL sets VTParser.bell, except where it ends an OSC or DCS
 *   vt_fuzz diff SEED N MODE small cells against full-size ones: same traffic, same result
 *                            (MODE 0 hostile, 1 mild, 2 mild with many different characters,
 *                            3 mild with many styles and wide characters at the margins)
 *   vt_fuzz SEED ITERATIONS  random escape-heavy traffic, split at random points,
 *                            with resizes; exit status 0 if nothing went wrong
 */
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t b[32]; int n; } Frag;
static const Frag frags[] = {
  {{0x1b,'['},2}, {{0x1b,']'},2}, {{0x1b,'P'},2}, {{0x1b,'('},2}, {{0x1b,'#'},2},
  {{0x1b,'7'},2}, {{0x1b,'8'},2}, {{0x1b,'c'},2}, {{0x1b,'\\'},2}, {{';'},1}, {{'?'},1},
  {{0x07},1}, {{'2',';'},2}, {{0xf0,0x9f,0x98,0x80},4}, {{0xe4,0xbd,0xa0},3}, {{0xc3,0xa9},2},
  {{0xff},1}, {{0x80},1},
  {{0x1b,'[','1','0','0','0','0','0','0','0','0','0','0','H'},15},
  {{0x1b,'[','9','9','9','9','9','9','9','9','9','9','9','9','m'},15},
  {{0x1b,'[','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','X'},24},
  {{0x1b,'[','?','1','0','4','9','h'},8},
  {{0x1b,'[','3','8',';','5',';','9','9','9','9','m'},12},
  {{0x1b,'[','4','8',';','2',';','2','5','5',';','0',';','0','m'},16},
  {{0x1b,']','0',';','x','y','z',0x07},7},
  {{0x1b,'[','@'},3}, {{0x1b,'[','S'},3}, {{0x1b,'[','T'},3}, {{0x1b,'[','L'},3}, {{0x1b,'[','M'},3},
  {{0x1b,'[','r'},3}, {{0x1b,'[','0',';','0','r'},5}, {{0x1b,'[','2','J'},4}, {{0x1b,'[','3','J'},4},
};
static const char alpha[] = "0123456789;:<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{|}~";

static int repro(void) {
    /* Save the cursor on the bottom row of a blank screen, move it to the top,
     * shrink the screen (which evicts rows into the scrollback), restore. */
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 50, 80);
    const char *a = "\x1b[50;1H\x1b" "7" "\x1b[1;1H";
    vt_feed(p, (const uint8_t *)a, strlen(a));
    vt_resize(p, 16, 80);
    const char *b = "\x1b" "8" "X";
    vt_feed(p, (const uint8_t *)b, strlen(b));
    int ok = p->scr.cur_row < p->scr.rows && p->scr.cur_col < p->scr.cols;
    vt_free(p); free(p);
    printf("%s: cursor after restore is inside the 16-row screen: %s\n", "repro", ok ? "yes" : "NO");
    return ok ? 0 : 1;
}

static int fuzz(unsigned seed, long iters) {
    srand(seed);
    VTParser *p = calloc(1, sizeof *p);
    int rows = 24, cols = 80;
    vt_init(p, rows, cols);
    for (long it = 0; it < iters; it++) {
        uint8_t buf[1024]; size_t n = 0;
        int k = 1 + rand() % 60;
        for (int j = 0; j < k && n < sizeof buf - 32; j++) {
            int r = rand() % 10;
            if (r < 5) { const Frag *f = &frags[rand() % (sizeof frags / sizeof frags[0])];
                         memcpy(buf + n, f->b, f->n); n += f->n; }
            else if (r < 9) buf[n++] = (uint8_t)alpha[rand() % (sizeof alpha - 1)];
            else buf[n++] = (uint8_t)rand();
        }
        size_t off = 0;                     /* split at random points: sequences straddle reads */
        while (off < n) { size_t take = 1 + rand() % (n - off); vt_feed(p, buf + off, take); off += take; }
        if (rand() % 300 == 0) { rows = 1 + rand() % 60; cols = 1 + rand() % 200; vt_resize(p, rows, cols); }
    }
    vt_free(p); free(p);
    printf("fuzz seed=%u iterations=%ld: no fault\n", seed, iters);
    return 0;
}

/* DEC special graphics: ESC ( 0 maps the line drawing letters, ESC ( B and a
 * full reset (RIS) take the characters back to ASCII. */
static int dec(void) {
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 20);
    const char *a = "\x1b(0lqqk\x1b(Bxq";
    vt_feed(p, (const uint8_t *)a, strlen(a));
    printf("dec: %u %u %u %u %u %u", vt_cell(&p->scr, 0, 0).ch, vt_cell(&p->scr, 0, 1).ch,
           vt_cell(&p->scr, 0, 2).ch, vt_cell(&p->scr, 0, 3).ch, vt_cell(&p->scr, 0, 4).ch, vt_cell(&p->scr, 0, 5).ch);
    const char *b = "\x1b(0q\x1b" "cq";
    vt_feed(p, (const uint8_t *)b, strlen(b));
    printf(" ris: %u\n", vt_cell(&p->scr, 0, 0).ch);
    vt_free(p); free(p);
    return 0;
}

/* A BEL in the ground state is a bell (VTParser.bell, cleared by the caller
 * after each feed here); a BEL that ends an OSC (BEL or ST) or a DCS string is
 * not. */
static int bell(void) {
    static const char *const in[] = { "x\a", "\x1b]0;t\a", "\x1b]0;t\x1b\\", "\x1bPq\a" };
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 20);
    printf("bell:");
    for (unsigned i = 0; i < sizeof in / sizeof *in; i++) {
        vt_feed(p, (const uint8_t *)in[i], strlen(in[i]));
        printf(" %d", (int)p->bell);
        p->bell = false;
    }
    printf("\n");
    vt_free(p); free(p);
    return 0;
}

/* The alternate screen: what is on the primary screen comes back after each
 * visit, the alt screen starts blank every time, and its grid is given back on
 * leaving (ag.row NULL), also across a resize while in it. */
static int alt(void) {
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 20);
    const char *a = "P\x1b[?1049hAAA\x1b[?1049l";
    int freed = 0, blank = 1;
    vt_feed(p, (const uint8_t *)a, strlen(a));
    freed += p->scr.ag.row == NULL;
    const char *b = "\x1b[?1049h";
    vt_feed(p, (const uint8_t *)b, strlen(b));
    blank = vt_cell(&p->scr, 0, 0).ch == ' ' && p->scr.in_alt_screen;
    vt_resize(p, 6, 30);
    const char *c = "Z\x1b[?1049lQ";
    vt_feed(p, (const uint8_t *)c, strlen(c));
    freed += p->scr.ag.row == NULL;
    printf("alt: primary %c%c blank %d freed %d in_alt %d\n", vt_cell(&p->scr, 0, 0).ch,
           vt_cell(&p->scr, 0, 1).ch, blank, freed, (int)p->scr.in_alt_screen);
    vt_free(p); free(p);
    return 0;
}


/* ---- differential test: small cells against full-size cells ----------------
 * The same traffic (the hostile kind, or a mild kind of ASCII, Latin-1, box
 * drawing, a few colours and attributes, scrolling, erasing, the alternate
 * screen and the odd wide character or resize, where grids stay small most of
 * the time) goes to two screens, one of which is made to use full-size cells
 * only; after every step they must hold the same cells (both grids, with the
 * defaults' colour bytes and a blank's two spellings equal), scrollback,
 * cursor and flags.  Exit status 0 if they did. */
static int cmp_grid(const Screen *a, const Screen *b, int ag, long it, const char *what) {
    Screen x = *a, y = *b;
    if (ag) { Grid t = x.g; x.g = x.ag; x.ag = t; t = y.g; y.g = y.ag; y.ag = t; }
    if ((x.g.row == NULL) != (y.g.row == NULL)) { printf("%s: grid presence differs at step %ld\n", what, it); return 1; }
    if (!x.g.row) return 0;
    for (int r = 0; r < a->rows; r++)
        for (int c = 0; c < a->cols; c++) {
            Cell p = vt_cell(&x, r, c), q = vt_cell(&y, r, c);
            if (p.flags & CELL_FG_DFL) p.fg = 0;
            if (q.flags & CELL_FG_DFL) q.fg = 0;
            if (p.flags & CELL_BG_DFL) p.bg = 0;
            if (q.flags & CELL_BG_DFL) q.bg = 0;
            if (p.ch != q.ch || p.fg != q.fg || p.bg != q.bg || p.attrs != q.attrs || (p.flags & 0x1f) != (q.flags & 0x1f)) {
                printf("%s: cell (%d,%d) differs at step %ld: ch %x/%x fg %u/%u bg %u/%u attrs %x/%x flags %x/%x\n",
                       what, r, c, it, p.ch, q.ch, p.fg, q.fg, p.bg, q.bg, p.attrs, q.attrs, p.flags, q.flags);
                return 1;
            }
        }
    return 0;
}

/* A wide character is never in halves: each first half has its second half after it
 * (with its style), each second half a first half before it. */
static int whole(const Screen *s, int ag, long it) {
    Screen x = *s;
    if (ag) { Grid t = x.g; x.g = x.ag; x.ag = t; }
    if (!x.g.row) return 1;
    for (int r = 0; r < s->rows; r++)
        for (int c = 0; c < s->cols; c++) {
            Cell p = vt_cell(&x, r, c), q = c + 1 < s->cols ? vt_cell(&x, r, c + 1) : p;
            int first = (p.flags & CELL_WIDE) != 0, second = (p.flags & CELL_WIDE_CONT) != 0;
            int prev = c > 0 && (vt_cell(&x, r, c - 1).flags & CELL_WIDE);
            if ((first && (c + 1 >= s->cols || !(q.flags & CELL_WIDE_CONT) || q.fg != p.fg || q.bg != p.bg || q.attrs != p.attrs)) ||
                (second && !prev) || (first && second)) {
                printf("half a wide character at (%d,%d)%s, step %ld\n", r, c, ag ? " of the other grid" : "", it);
                return 0;
            }
        }
    return 1;
}

static int same(const VTParser *a, const VTParser *b, long it) {
    const Screen *s = &a->scr, *t = &b->scr;
    if (!whole(s, 0, it) || !whole(s, 1, it) || !whole(t, 0, it) || !whole(t, 1, it)) return 0;
    if (s->rows != t->rows || s->cols != t->cols || s->cur_row != t->cur_row || s->cur_col != t->cur_col ||
        s->in_alt_screen != t->in_alt_screen || s->scroll_top != t->scroll_top || s->scroll_bottom != t->scroll_bottom ||
        s->pending_wrap != t->pending_wrap) { printf("state differs at step %ld\n", it); return 0; }
    if (s->scrollback_len != t->scrollback_len || (s->scrollback_len && memcmp(s->scrollback, t->scrollback, (size_t)s->scrollback_len))) {
        printf("scrollback differs at step %ld\n", it); return 0; }
    if (s->row_flags && memcmp(s->row_flags, t->row_flags, (size_t)s->rows)) { printf("row flags differ at step %ld\n", it); return 0; }
    return !cmp_grid(s, t, 0, it, "grid") && !cmp_grid(s, t, 1, it, "other grid");
}

static const char *const mild[] = {
  "hello world ", "The quick brown fox\r\n", "\r\n", "\r\n", "\t", "\b", "x", "yy", "ABC",
  "\xc3\xa9\xc3\xbc\xc3\x9f", "\xc5\x82\xc5\xbc",                                  /* Latin-1, Latin-2 */
  "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90", "\xe2\x94\x82", "\xe2\x94\x94\xe2\x94\x98", /* box drawing */
  "\x1b[1m", "\x1b[7m", "\x1b[4m", "\x1b[0m", "\x1b[22m", "\x1b[27m", "\x1b[2m", "\x1b[3m",
  "\x1b[31m", "\x1b[32;44m", "\x1b[33m", "\x1b[1;37;41m", "\x1b[39m", "\x1b[49m", "\x1b[90m", "\x1b[38;5;208m",
  "\x1b[2J", "\x1b[H", "\x1b[3J", "\x1b[K", "\x1b[1K", "\x1b[2K", "\x1b[J", "\x1b[1J", "\x1b[3X",
  "\x1b[2@", "\x1b[3P", "\x1b[L", "\x1b[2M", "\x1b[S", "\x1b[T", "\x1b[5;20r", "\x1b[r",
  "\x1b[?1049h", "\x1b[?1049l", "\x1b[?1049h", "\x1b[?1049l", "\x1b[4h", "\x1b[4l",
  "\x1b[3;7H", "\x1b[10;1H", "\x1b[A", "\x1b[3B", "\x1b[2C", "\x1b[D", "\x1b[3b", "\x1b" "7", "\x1b" "8",
  "\xe3\x83\x84", "\xf0\x9f\x98\x80", "\xe2\x9c\x85", "\xf0\x9f\x9a\x80",       /* wide ones the font has: ツ 😀 ✅ 🚀 */
  "\xe4\xbd\xa0", "\xef\xbc\xa1", "\xf0\x9f\xa7\x91",                   /* wide ones it has not (rare below): 你 Ａ 🧑 */
};

static int diff(unsigned seed, long iters, int mode) {
    srand(seed);
    VTParser *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    int rows = 24, cols = 80;
    vt_compact = 1; vt_init(a, rows, cols);
    vt_compact = 0; vt_init(b, rows, cols);
    long small = 0, steps = 0;
    for (long it = 0; it < iters; it++) {
        uint8_t buf[1024]; size_t n = 0;
        int k = 1 + rand() % 40;
        for (int j = 0; j < k && n < sizeof buf - 32; j++) {
            if (mode == 0) {
                int r = rand() % 10;
                if (r < 5) { const Frag *f = &frags[rand() % (sizeof frags / sizeof frags[0])]; memcpy(buf + n, f->b, f->n); n += f->n; }
                else if (r < 9) buf[n++] = (uint8_t)alpha[rand() % (sizeof alpha - 1)];
                else buf[n++] = (uint8_t)rand();
            } else {
                int m = (int)(sizeof mild / sizeof *mild) - 3;      /* the last three are wide: one in 40 */
                const char *t = mild[rand() % (rand() % 40 ? m : m + 3)];
                size_t l = strlen(t); memcpy(buf + n, t, l); n += l;
                if (mode == 3) {            /* many styles; wide characters wherever they fall */
                    char o[48];
                    int l = snprintf(o, sizeof o, "\x1b[%d;%d;%dm%s\x1b[%dG", rand() % 3 ? 38 : 48, 5, rand() % 256,
                                     rand() % 3 ? "x" : (rand() % 2 ? "\xe3\x83\x84" : "\xf0\x9f\x98\x80"), 1 + rand() % 90);
                    if (rand() % 4) l = snprintf(o, sizeof o, "\x1b[%d;%dm", 30 + rand() % 8, 40 + rand() % 8);
                    memcpy(buf + n, o, (size_t)l); n += (size_t)l;
                }
                if (mode == 2 && rand() % 4 == 0) { int c = 0x80 + rand() % 300; buf[n++] = 0xc0 | (c >> 6); buf[n++] = 0x80 | (c & 63); }
            }
        }
        size_t off = 0;
        while (off < n) { size_t take = 1 + rand() % (n - off);
            vt_compact = 1; vt_feed(a, buf + off, take); vt_compact = 0; vt_feed(b, buf + off, take); off += take; }
        if (rand() % 200 == 0) { rows = 1 + rand() % 40; cols = 2 + rand() % 120;
            vt_compact = 1; vt_resize(a, rows, cols); vt_compact = 0; vt_resize(b, rows, cols); }
        steps++; small += a->scr.g.t != NULL;
        if (!same(a, b, it)) {
            printf("  last input:");
            for (size_t i = 0; i < n; i++) printf(buf[i] >= 0x20 && buf[i] < 0x7f ? "%c" : "<%02x>", buf[i]);
            printf("\n");
            return 1;
        }
    }
    vt_compact = 1; vt_free(a); vt_compact = 0; vt_free(b); free(a); free(b);
    printf("diff seed=%u mode=%d steps=%ld: identical, grid small in %ld%% of them\n", seed, mode, steps, small * 100 / steps);
    return 0;
}

/* The style table of a small grid: 127 styles on the screen and a scroll by three lines
 * that needs a 128th for the blank lines (it used to leave rows pointing into a freed grid, and
 * is what ASan is there for); then a window that sees hundreds of styles but never more than a
 * few at a time, which must stay small. */
static int styles(void) {
    VTParser *p = calloc(1, sizeof *p);
    char b[64];
    vt_init(p, 24, 80);
    for (int i = 0; i < 127; i++) {
        int n = snprintf(b, sizeof b, "\x1b[38;5;%dm\x1b[48;5;%dmx", i, 255 - i);
        vt_feed(p, (const uint8_t *)b, (size_t)n);
    }
    const char *a = "\x1b[38;5;200m\x1b[48;5;201m\x1b[3S";
    vt_feed(p, (const uint8_t *)a, strlen(a));
    int widened = p->scr.g.t == NULL;
    vt_free(p);
    vt_init(p, 24, 80);
    for (int i = 0; i < 1000; i++) {
        int n = snprintf(b, sizeof b, "\x1b[38;5;%d;48;5;%dmline %d\r\n", i % 250, (i * 7) % 250, i);
        vt_feed(p, (const uint8_t *)b, (size_t)n);
    }
    int small = p->scr.g.t != NULL;
    vt_free(p); free(p);
    printf("styles: full table widened %d, many styles in turn stayed small %d\n", widened, small);
    return 0;
}

/* Invalid UTF-8 draws nothing, and costs the text after it nothing. */
static int utf8(void) {
    static const char *const in[] = {
        "a\xc0\x80" "b",            /* overlong NUL */
        "a\xed\xa0\x80" "b",       /* surrogate */
        "a\xf4\x90\x80\x80" "b",  /* past U+10FFFF */
        "a\xe0\x80\xaf" "b",       /* overlong slash */
        "\xc3" "x",                /* cut short by ASCII */
        "\xe2\x94" "x\xc3\xa9",   /* cut short, then a good one */
        "\xe9\r\nxy",              /* cut short by controls */
        "\xe2\x1b[1m" "z",          /* cut short by an escape sequence */
        "\xc3\xa9\xe2\x94\x80",    /* good ones: unharmed */
    };
    for (unsigned i = 0; i < sizeof in / sizeof *in; i++) {
        VTParser *p = calloc(1, sizeof *p);
        vt_init(p, 3, 10);
        vt_feed(p, (const uint8_t *)in[i], strlen(in[i]));
        printf("[");
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 10; c++) { uint32_t ch = vt_cell(&p->scr, r, c).ch; if (ch != ' ') printf(ch < 128 ? "%c" : "<%x>", ch < 128 ? (int)ch : (int)ch); }
            if (!r) printf("|");
        }
        printf("]");
        vt_free(p); free(p);
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "repro")) return repro();
    if (argc == 2 && !strcmp(argv[1], "dec")) return dec();
    if (argc == 2 && !strcmp(argv[1], "bell")) return bell();
    if (argc == 2 && !strcmp(argv[1], "alt")) return alt();
    if (argc == 2 && !strcmp(argv[1], "utf8")) return utf8();
    if (argc == 2 && !strcmp(argv[1], "styles")) return styles();
    if (argc == 5 && !strcmp(argv[1], "diff")) return diff((unsigned)atoi(argv[2]), atol(argv[3]), atoi(argv[4]));
    if (argc == 3) return fuzz((unsigned)atoi(argv[1]), atol(argv[2]));
    fprintf(stderr, "usage: vt_fuzz repro | dec | bell | alt | styles | utf8 | diff SEED N MODE | vt_fuzz SEED ITERATIONS\n");
    return 2;
}
