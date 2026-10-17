/*
 * vt_check -- the primary screen's text against a model, for the VT layer
 * (vt.c) alone.  Printing, newlines and resizes in random order; after each
 * step the logical lines the scrollback and the screen hold must be the
 * lines printed, and the cursor must sit just after the last of them,
 * pending-wrap included.  A few fixed scenarios are checked first.
 *
 * Build and run from the repository root:
 *   diet gcc -I_lib -I_font -I_sys -o vt_check xtmux/tests/vt_check.c _lib/vt.c
 *   ./vt_check [seeds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt.h"

static int failures;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; \
    printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void feed(VTParser *p, const char *s) { vt_feed(p, (const uint8_t *)s, strlen(s)); }

/* ------------------------------------------------------------------ */
/* Fixed scenario: the reflow the user described                       */
/* ------------------------------------------------------------------ */
static bool row_is(const VTParser *p, int r, const char *text, int flag)
{
    char got[256]; int k = 0;
    for (int c = 0; c < p->scr.cols && k < 255; c++)
        if (!(p->scr.cells[r][c].flags & CELL_WIDE_CONT))
            got[k++] = (char)(p->scr.cells[r][c].ch ? p->scr.cells[r][c].ch : ' ');
    got[k] = 0;
    while (k > 0 && got[k - 1] == ' ') got[--k] = 0;
    int wrapped = p->scr.row_flags[r] & ROW_WRAPPED ? 1 : 0;
    if (strcmp(got, text) != 0 || wrapped != flag) {
        printf("    row %d: got \"%s\"%s, want \"%s\"%s\n", r, got, wrapped ? " (W)" : "",
               text, flag ? " (W)" : "");
        return false;
    }
    return true;
}

static int recover(const VTParser *p, char **out, int max);
static bool row_is_wrapped(const VTParser *p, int r) { return (p->scr.row_flags[r] & ROW_WRAPPED) != 0; }

static void scenario_reflow(void)
{
    printf("reflow scenario\n");
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 6, 10);
    feed(p, "0123456789ABC\r\nL1\r\nL2");                 /* a line one row and 3 cells long */
    EXPECT(row_is(p, 0, "0123456789", 1) && row_is(p, 1, "ABC", 0), "wrapped at 10 columns");

    vt_resize(p, 6, 20);                                  /* widen: the line fits on one row */
    EXPECT(row_is(p, 0, "0123456789ABC", 0) && row_is(p, 1, "L1", 0) && row_is(p, 2, "L2", 0),
           "after widening to 20: one row, L1 and L2 moved up");
    EXPECT(p->scr.cur_row == 2 && p->scr.cur_col == 2, "cursor after L2 (row 2, col 2), got %d,%d",
           p->scr.cur_row, p->scr.cur_col);

    vt_resize(p, 6, 5);                                   /* narrow: it spills over 3 rows */
    EXPECT(row_is(p, 0, "01234", 1) && row_is(p, 1, "56789", 1) && row_is(p, 2, "ABC", 0) &&
           row_is(p, 3, "L1", 0) && row_is(p, 4, "L2", 0),
           "after narrowing to 5: 3 rows, L1 and L2 moved down");
    EXPECT(p->scr.cur_row == 4 && p->scr.cur_col == 2, "cursor after L2 at 5 columns, got %d,%d",
           p->scr.cur_row, p->scr.cur_col);
    EXPECT(p->scr.scrollback_len == 0, "nothing scrolled off");
    vt_free(p);
    free(p);
}

/* A wide character that does not fit at the margin leaves padding there; the
 * padding is not text, and it goes away when the line is laid out again. */
static void scenario_wide(void)
{
    printf("wide character scenario\n");
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 4);
    feed(p, "abc\xe4\xb8\xad" "d\r\nxy");                /* 3 cells, then a wide one, then d */
    char *got[8];
    int n = recover(p, got, 8);
    EXPECT(n == 2 && strcmp(got[0], "abc\xe4\xb8\xad" "d") == 0, "text at 4 columns");
    for (int i = 0; i < n; i++) free(got[i]);
    EXPECT(p->scr.row_flags[0] & ROW_WRAPPED, "the row before the wide character is flagged");
    EXPECT(p->scr.cells[0][3].flags & CELL_PAD, "the margin cell is padding");

    vt_resize(p, 4, 8);                                   /* widen: the padding goes */
    n = recover(p, got, 8);
    EXPECT(n == 2 && strcmp(got[0], "abc\xe4\xb8\xad" "d") == 0, "text at 8 columns");
    for (int i = 0; i < n; i++) free(got[i]);
    EXPECT(p->scr.cells[0][3].ch == 0x4E2D && !(p->scr.cells[0][3].flags & CELL_PAD),
           "the wide character sits right after abc");
    EXPECT(!(p->scr.row_flags[0] & ROW_WRAPPED), "and the line is one row");

    vt_resize(p, 4, 3);                                   /* narrow: abc fills its row */
    EXPECT(row_is_wrapped(p, 0) && p->scr.cells[1][0].ch == 0x4E2D,
           "at 3 columns, abc fills row 0 and 中d starts row 1");
    vt_free(p);
    free(p);
}

/* A shell repaints its input after a resize (bash, on SIGWINCH): the
 * captured exchange.  The input is a 27-character prompt and a typed command,
 * 138 cells on a 40-column screen; the screen goes to 25 columns, and bash
 * moves up the three rows it drew the input over (its count was made at 40
 * columns), clears them and draws the input again.  The prompt must appear
 * once, and the cursor must end after the last character. */
static void scenario_readline(void)
{
    printf("readline repaint scenario\n");
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 24, 40);
    feed(p, "[ps1-long-prompt-here]$ echo word00 word01 word02 word03 word04 word05 word06 word07 word08 word09 word10 word11 word12 word13");
    vt_resize(p, 24, 25);
    feed(p, "\r\x1b[K\r\x1b[A\x1b[K\r\x1b[A\x1b[K\r\x1b[A\x1b[K\r[ps1-long-prompt-here]$ echo word00 word01 word02 word03 word04 word05 word06 word07 word08 word09 word10 word11 word12 word13");
    EXPECT(row_is(p, 0, "[ps1-long-prompt-here]$ e", 1) && row_is(p, 5, "3", 0),
           "the prompt is drawn once, from the top of the input");
    EXPECT(p->scr.cur_row == 5 && p->scr.cur_col == 1, "cursor after the last character, got %d,%d",
           p->scr.cur_row, p->scr.cur_col);
    vt_free(p);
    free(p);
}

/* A line that exactly fills a row, then a newline, is not a wrapped line. */
static void scenario_exact_fill(void)
{
    printf("exact-fill scenario\n");
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 10);
    feed(p, "0123456789\r\nAB");
    EXPECT(row_is(p, 0, "0123456789", 0), "a full row that ends in a newline is not flagged");
    vt_resize(p, 4, 20);
    EXPECT(row_is(p, 0, "0123456789", 0) && row_is(p, 1, "AB", 0),
           "widening keeps the two lines apart");
    vt_free(p);
    free(p);
}

/* REP (CSI Ps b) repeats the last graphic character with the current
 * rendition.  irssi paints its status bar this way: one blank with a blue
 * background, then ESC [ 79 b; without REP the rest of the bar stays blank
 * in the default colour. */
static void scenario_rep(void)
{
    printf("repeat scenario\n");
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 4, 10);
    feed(p, "\x1b[44m \x1b[9b\x1b[0m");                  /* ten blue blanks in all */
    int blue = 0;
    for (int c = 0; c < 10; c++)
        if (!(p->scr.cells[0][c].flags & CELL_BG_DFL) && p->scr.cells[0][c].bg == 4) blue++;
    EXPECT(blue == 10, "REP paints the whole row blue, got %d of 10 cells", blue);
    EXPECT(p->scr.cur_row == 0 && p->scr.cur_col == 9 && p->scr.pending_wrap,
           "cursor left at the margin with pending wrap, got %d,%d", p->scr.cur_row, p->scr.cur_col);

    feed(p, "\r\ny\x1b[b");                              /* no count: one more */
    EXPECT(row_is(p, 1, "yy", 0), "REP with no count repeats once");
    feed(p, "\x1b[0b");                                  /* count 0 is read as 1 */
    EXPECT(row_is(p, 1, "yyy", 0), "REP with count 0 repeats once");

    feed(p, "\r\nz\x1b[12b");                            /* runs past the margin */
    EXPECT(row_is(p, 2, "zzzzzzzzzz", 1) && row_is(p, 3, "zzz", 0),
           "REP wraps at the margin like any printed text");

    VTParser *q = calloc(1, sizeof *q);
    vt_init(q, 4, 10);
    feed(q, "\x1b[5b");                                  /* nothing printed yet */
    EXPECT(q->scr.cur_row == 0 && q->scr.cur_col == 0 && row_is(q, 0, "", 0),
           "REP before any character prints nothing");
    vt_free(q);
    free(q);

    vt_free(p);
    free(p);
}

/* ------------------------------------------------------------------ */
/* Random printing and resizing, against a model                       */
/* ------------------------------------------------------------------ */
#define MAX_LINES 4096

typedef struct {
    char *lines[MAX_LINES];   /* complete lines, in order */
    int   n;
    char  cur[1024];          /* the line the cursor is on */
} Model;

static char *dupstr(const char *s) { char *d = malloc(strlen(s) + 1); strcpy(d, s); return d; }

static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

#define CJK 0x4E2D   /* a wide character (2 cells) the random text uses */

/* Rows, and the cursor's column and pending state, for the text s laid out
 * at nc columns the way the VT lays a line out: a character that does not
 * fit wraps first (and a wide one leaves its padding). */
static void layout_text(const char *s, int nc, int *rows, int *col, int *pend)
{
    int r = 0, c = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        uint32_t cp; int len;
        if (*u < 0x80)      { cp = *u; len = 1; }
        else if (*u < 0xE0) { cp = *u & 0x1F; len = 2; }
        else if (*u < 0xF0) { cp = *u & 0x0F; len = 3; }
        else                { cp = *u & 0x07; len = 4; }
        for (int k = 1; k < len; k++) cp = (cp << 6) | (u[k] & 0x3F);
        u += len;
        int w = cp == CJK ? 2 : 1;
        if (c + w > nc) { r++; c = 0; }
        c += w;
    }
    *rows = r + 1;
    *col = c;
    *pend = 0;
    if (c == nc) { *col = nc - 1; *pend = 1; }   /* the last character filled the row */
}

static void rstrip(char *s) { size_t n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0; }

/* The lines the VT holds: the scrollback's text, then the rows down to the
 * cursor's, a row without ROW_WRAPPED ending its line.  Each is rstripped. */
static int recover(const VTParser *p, char **out, int max)
{
    const Screen *s = &p->scr;
    size_t cap = (size_t)s->scrollback_len + (size_t)s->rows * (s->cols * 3 + 1) + 2;
    char *st = malloc(cap), *q = st;
    memcpy(q, s->scrollback, (size_t)s->scrollback_len); q += s->scrollback_len;
    int last = s->cur_row;             /* a line can continue below the cursor (see vt.c, reflow) */
    for (int r = s->cur_row + 1; r < s->rows; r++) {
        if (s->row_flags[r] & ROW_WRAPPED) { last = r; continue; }
        for (int c = 0; c < s->cols; c++)
            if (s->cells[r][c].ch && s->cells[r][c].ch != ' ' && !(s->cells[r][c].flags & CELL_PAD)) last = r;
    }
    for (int r = 0; r <= last; r++) {   /* rows down to the last with text (or a wrap) */
        for (int c = 0; c < s->cols; c++) {
            if (s->cells[r][c].flags & (CELL_WIDE_CONT | CELL_PAD)) continue;
            uint32_t ch = s->cells[r][c].ch;
            q += utf8_put(q, ch ? ch : ' ');
        }
        if (!(s->row_flags[r] & ROW_WRAPPED)) *q++ = '\n';
    }
    if (q > st && q[-1] != '\n') *q++ = '\n';
    *q = 0;
    int n = 0;
    for (char *a = st, *b; a && *a && n < max; a = b ? b + 1 : NULL) {
        b = strchr(a, '\n');
        if (!b) break;                  /* the stream always ends with '\n' */
        *b = 0;
        out[n++] = dupstr(a);
        rstrip(out[n - 1]);
    }
    free(st);
    return n;
}

static int check(const VTParser *p, const Model *m, long step, const char *what, int cursor)
{
    static char *got[MAX_LINES];
    int n = recover(p, got, MAX_LINES);
    int want = m->n + 1;                         /* the current line counts, even if empty */
    int ok = (n == want);
    for (int i = 0; ok && i < m->n; i++) {
        char w[1024]; strcpy(w, m->lines[i]); rstrip(w);
        if (strcmp(got[i], w) != 0) ok = 0;
    }
    if (ok) {
        char w[1024]; strcpy(w, m->cur); rstrip(w);
        /* After a resize the cursor's line is not reflowed (see vt.c's reflow): it
         * is cut or padded until the shell repaints it, so it is only checked then. */
        if (cursor && strcmp(got[n - 1], w) != 0) ok = 0;
    }
    if (!ok) {
        printf("  FAIL after %s (step %ld): VT holds %d lines, model %d\n", what, step, n, want);
        for (int i = 0; i < n && i < want + 2; i++)
            printf("    VT %2d \"%s\"   model %2d \"%s\"\n", i, got[i], i,
                   i < m->n ? m->lines[i] : i == m->n ? m->cur : "");
        failures++;
    }
    if (!cursor) { for (int i = 0; i < n; i++) free(got[i]); return ok; }
    /* The cursor: after the last character of the current line.  If the head
     * of the line is in the scrollback (the open tail, which row 0 continues),
     * its rows are not on the screen. */
    const Screen *s = &p->scr;
    int r0 = s->cur_row;
    while (r0 > 0 && (s->row_flags[r0 - 1] & ROW_WRAPPED)) r0--;
    char head[1024] = "";
    if (r0 == 0) {
        int k = s->scrollback_len, h = 0;
        while (k > 0 && s->scrollback[k - 1] != '\n') k--;
        for (int i = k; i < s->scrollback_len && h < 1023; i++) head[h++] = s->scrollback[i];
        head[h] = 0;
    }
    int rows_all, col_exp, pend_exp, rows_head, dummy_col, dummy_pend;
    layout_text(m->cur, s->cols, &rows_all, &col_exp, &pend_exp);
    if (r0 == 0 && head[0]) layout_text(head, s->cols, &rows_head, &dummy_col, &dummy_pend);
    else rows_head = 0;
    int rows_on_screen = rows_all - rows_head;
    if (s->cur_row - r0 != rows_on_screen - 1 || s->cur_col != col_exp || s->pending_wrap != pend_exp) {
        printf("  FAIL after %s (step %ld): cursor at %d,%d%s, want row %d col %d%s (line of %d rows, %d in the scrollback)\n",
               what, step, s->cur_row, s->cur_col, s->pending_wrap ? " pending" : "",
               r0 + rows_on_screen - 1, col_exp, pend_exp ? " pending" : "", rows_all, rows_head);
        failures++;
        ok = 0;
    }
    for (int i = 0; i < n; i++) free(got[i]);
    return ok;
}

static void model_feed(Model *m, const char *s)
{
    for (; *s; s++) {
        if (*s == '\r') continue;
        if (*s == '\n') {
            if (m->n < MAX_LINES - 1) m->lines[m->n++] = dupstr(m->cur);
            m->cur[0] = 0;
        } else {
            size_t l = strlen(m->cur);
            if (l + 2 < sizeof m->cur) { m->cur[l] = *s; m->cur[l + 1] = 0; }
        }
    }
}

static void model_free(Model *m) { for (int i = 0; i < m->n; i++) free(m->lines[i]); m->n = 0; }

static unsigned rng;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

/*
 * A shell at its prompt (readline) repaints its input after a resize: it
 * moves up by the rows it last drew the input over, clears them and draws
 * the input again.  The random run does the same: a burst of resizes, then
 * (maybe) a repaint, and prints only after a repaint, as a shell would.
 */
static void repaint(VTParser *p, Model *m, int v_read)
{
    char buf[2048]; int k = 0;
    k += snprintf(buf + k, sizeof buf - k, "\r\x1b[K");
    for (int i = 0; i < v_read; i++) k += snprintf(buf + k, sizeof buf - k, "\r\x1b[A\x1b[K");
    k += snprintf(buf + k, sizeof buf - k, "\r");
    memcpy(buf + k, m->cur, strlen(m->cur)); k += (int)strlen(m->cur);
    vt_feed(p, (const uint8_t *)buf, (size_t)k);
}

static void random_run(unsigned seed, int steps)
{
    rng = seed;
    VTParser *p = calloc(1, sizeof *p);
    int rows = 3 + (int)rnd(8), cols = 4 + (int)rnd(20);
    vt_init(p, rows, cols);
    Model m; memset(&m, 0, sizeof m);
    int v_read = 0;                 /* the rows below the input's first row, as the shell knows them */
    int pending = 0;                /* resized since the last repaint: the shell has not redrawn yet */
    int fails0 = failures;
    for (int step = 0; step < steps && failures == fails0; step++) {
        char what[64];
        /* A repaint moves up v_read rows; the screen has only rows-1 above the
         * bottom row, so when the input is taller than the screen that move
         * stops at the top, and its first rows stay in the history: not a case
         * the screen can show (see the notes in vt.c's reflow). */
        /* The shell's input must start on the screen: an open line in the scrollback
         * is the start of it, and its rows are not counted here (see vt.c's reflow). */
        const int open_line = p->scr.scrollback_len > 0 &&
                              p->scr.scrollback[p->scr.scrollback_len - 1] != '\n';
        if (pending && rnd(100) < 40 && v_read < p->scr.rows && !open_line) {
            repaint(p, &m, v_read);
            int rr, cc, pp; layout_text(m.cur, p->scr.cols, &rr, &cc, &pp);
            v_read = rr - 1;
            pending = 0;
            snprintf(what, sizeof what, "repaint");
        } else if (rnd(100) < 25 || pending) {
            int nr = 2 + (int)rnd(11), nc = 2 + (int)rnd(26);
            vt_resize(p, nr, nc);
            pending = 1;
            snprintf(what, sizeof what, "resize %dx%d", nr, nc);
        } else {
            int len = 1 + (int)rnd(30), k = 0;
            char buf[512];
            for (int i = 0; i < len; i++) {
                unsigned r = rnd(100);
                if (r < 14)      buf[k++] = '\n';          /* newline; CR is added before it */
                else if (r < 16) { buf[k++] = '\r'; buf[k++] = '\n'; }
                else if (r < 19) k += utf8_put(buf + k, CJK);
                else              buf[k++] = (char)('a' + rnd(26));
            }
            buf[k] = 0;
            char out[1536]; int o = 0;
            for (int i = 0; i < k; i++) {
                if (buf[i] == '\n') out[o++] = '\r';
                out[o++] = buf[i];
            }
            out[o] = 0;
            feed(p, out);
            model_feed(&m, buf);
            int rr, cc, pp; layout_text(m.cur, p->scr.cols, &rr, &cc, &pp);
            v_read = rr - 1;
            snprintf(what, sizeof what, "print %d chars", len);
        }
        if (getenv("VT_TRACE") && (unsigned)atoi(getenv("VT_TRACE")) == seed) {
            printf("  step %d %s -> cursor %d,%d%s rows %dx%d\n", step, what, p->scr.cur_row,
                   p->scr.cur_col, p->scr.pending_wrap ? "p" : "", p->scr.rows, p->scr.cols);
            printf("      scrollback tail: [");
            for (int k = p->scr.scrollback_len > 30 ? p->scr.scrollback_len - 30 : 0; k < p->scr.scrollback_len; k++)
                putchar(p->scr.scrollback[k] == '\n' ? '|' : p->scr.scrollback[k]);
            printf("]\n");
            for (int r = 0; r < p->scr.rows; r++) {
                printf("      %c |", p->scr.row_flags[r] & ROW_WRAPPED ? 'W' : ' ');
                for (int c = 0; c < p->scr.cols; c++) { uint32_t ch = p->scr.cells[r][c].ch; putchar(ch && ch < 0x80 ? (int)ch : '.'); }
                printf("|\n");
            }
        }
        /* After a resize the text must be intact; the cursor is checked once the shell has drawn. */
        check(p, &m, step, what, !pending);
        if (m.n > 300) break;   /* keep the scrollback below its size limit */
    }
    if (failures != fails0) printf("  seed %u (started %d x %d)\n", seed, rows, cols);
    model_free(&m);
    vt_free(p);
    free(p);
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 300;
    scenario_reflow();
    scenario_exact_fill();
    scenario_rep();
    scenario_wide();
    scenario_readline();
    for (unsigned s = 1; s <= (unsigned)seeds; s++) random_run(s, 400);
    printf("%s: %d failure(s) in %d seeds\n", failures ? "FAILED" : "all passed", failures, seeds);
    return failures != 0;
}
