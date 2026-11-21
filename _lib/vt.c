#include "vt.h"
#include "width.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

/*
 * CSI parameter accessor.
 * Returns the i-th parameter if it was explicitly set and non-zero,
 * otherwise returns `def`.  (VT100 convention: 0 == "use default".)
 */
#define P(i, def) ((i) < p->num_params && p->params[(i)] > 0 \
                   ? p->params[(i)] : (def))

/* ------------------------------------------------------------------ */
/* Cell / row helpers                                                   */
/* ------------------------------------------------------------------ */

/* Is grid g in the small format?  (Never, if that is configured out.) */
#if VT_CELL_COMPACT
#define GT(g) ((g)->t)
int vt_compact = 1;     /* 0: new grids are full-size (for tests) */
#else
#define GT(g) 0
#endif
static inline size_t gesz(const Grid *g) { return GT(g) ? 2 : sizeof(Cell); }

static Cell blank_cell(const Screen *s)
{
    Cell c = {0};
    c.ch    = ' ';
    if (s->cur_fg == COLOR_DEFAULT) {
        c.flags |= CELL_FG_DFL;
    } else {
        c.fg = (uint8_t)s->cur_fg;
    }
    if (s->cur_bg == COLOR_DEFAULT) {
        c.flags |= CELL_BG_DFL;
    } else {
        c.bg = (uint8_t)s->cur_bg;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Cell grid allocation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Cell grid layout: one contiguous allocation holds both the row-pointer
 * array and the cell data, making the whole thing one free() call and —
 * crucially — one contiguous region that madvise() can act on usefully.
 *
 *   [ Cell* [rows] ][ Cell [rows*cols] ]
 *     ^--- grid       ^--- cells_data(grid, rows)
 *
 * sizeof(Cell*)==8 and sizeof(Cell)==8, so alignment is always satisfied.
 */
static inline Cell *cells_data(Cell **grid, int rows)
{
    return (Cell *)((char *)grid + (size_t)rows * sizeof(Cell *));
}

static inline size_t cells_data_bytes(int rows, int cols)
{
    return (size_t)rows * (size_t)cols * sizeof(Cell);
}

static Cell **alloc_cells(int rows, int cols)
{
    size_t ptr_sz  = (size_t)rows * sizeof(Cell *);
    size_t data_sz = cells_data_bytes(rows, cols);
    Cell **grid    = calloc(1, ptr_sz + data_sz);
    if (!grid) return NULL;

    Cell *data = cells_data(grid, rows);
    for (int r = 0; r < rows; r++) {
        grid[r] = data + r * cols;
        for (int c = 0; c < cols; c++) {
            grid[r][c].ch    = ' ';
            grid[r][c].flags = CELL_FG_DFL | CELL_BG_DFL;
        }
    }
    return grid;
}

static void free_cells(Cell **grid, int rows)
{
    (void)rows;   /* no longer needed – single allocation */
    free(grid);
}

/* ---- madvise helpers (Linux only) ---- */

#ifdef __linux__
#include <sys/mman.h>

/* Round a pointer UP to the next page boundary. */
static inline void *page_align_up(void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (void *)((v + 4095u) & ~(uintptr_t)4095u);
}

/* Round a pointer DOWN to the previous page boundary. */
static inline void *page_align_down(void *p)
{
    return (void *)((uintptr_t)p & ~(uintptr_t)4095u);
}

/*
 * Apply an madvise hint to the page-aligned interior of [buf, buf+len).
 * Rounds inward so we never advise pages that belong to neighbouring
 * allocations.  Does nothing if the region is smaller than one page.
 */
static void cells_madvise(void *buf, size_t len, int advice)
{
    void *start = page_align_up(buf);
    void *end   = page_align_down((char *)buf + len);
    if (end <= start) return;
    madvise(start, (size_t)((char *)end - (char *)start), advice);
}

/* Tell the kernel the data pages backing a cell grid are no longer
 * needed.  Call immediately before free_cells(). */
static void cells_dontneed(const Grid *g, int rows, int cols)
{
    cells_madvise((char *)g->row + (size_t)rows * sizeof(void *),
                  (size_t)rows * (size_t)cols * gesz(g), MADV_DONTNEED);
}

/* Tell the kernel the data pages of an inactive (alt) grid are cold
 * and can be deprioritised in the LRU.  MADV_COLD is a soft hint:
 * the data is still valid if accessed; the kernel just treats these
 * pages as candidates for reclaim under memory pressure.
 * Requires Linux 5.4+; guarded so older headers compile cleanly. */
static void cells_cold(const Grid *g, int rows, int cols)
{
#if defined(MADV_COLD)
    cells_madvise((char *)g->row + (size_t)rows * sizeof(void *),
                  (size_t)rows * (size_t)cols * gesz(g), MADV_COLD);
#else
    (void)g; (void)rows; (void)cols;
#endif
}

#else  /* !__linux__ */
static void cells_dontneed(const Grid *g, int rows, int cols)
{ (void)g; (void)rows; (void)cols; }
static void cells_cold(const Grid *g, int rows, int cols)
{ (void)g; (void)rows; (void)cols; }
#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* Grid formats: full-size cells, and small ones (VT_CELL_COMPACT)      */
/* ------------------------------------------------------------------ */

/*
 * A full-size grid (Grid.t == NULL) is rows of Cell.  A small one is rows of
 * uint16_t, and Grid.t holds what the style numbers in them stand for.  A
 * small cell is a glyph number (9 bits) and a style number (7 bits):
 *
 *   glyph number     for a narrow character, its slot in the font: 0..94 are
 *                    U+0020..U+007E, 95..190 are U+00A0..U+00FF, then
 *                    charset_extra[] (_font/charset.h), the characters the
 *                    font draws: up to 511 of them, 0..510.
 *                    511 is not a glyph: it says that this cell is the first
 *                    half of a double-width character, and the cell after it
 *                    is not a cell but the character: all 16 bits of it are
 *                    the index of its glyph, 0..255 in charset_wide_extra[],
 *                    256.. in emoji_charset[] (at most 255 of them, so that
 *                    that value is never 511).  That is 511 + 511 glyphs in
 *                    nine bits, and a wide character is two cells as it is in
 *                    a full-size grid.
 *   style number     an index into t->st: fg, bg, the attributes and the
 *                    default-colour and padding flags, as seen so far on this
 *                    grid (up to 128; 0 is the defaults and nothing else).  A
 *                    wide character has the style of its first cell: the
 *                    second half is drawn with it.
 *
 * so an all-zero cell is a plain blank, and a freshly calloc()ed grid is a
 * blank screen whose pages cost nothing until they are written to.  The
 * glyph tables are compile-time and shared, so a small grid only has styles
 * to look up.
 *
 * The one thing a cell does not tell is whether it is the second half of a
 * wide character: that is the cell before it being a 511.  So a wide
 * character is never left in halves, in either kind of grid: whatever
 * overwrites, erases, shifts or cuts off one half of it first blanks the other
 * (cut(), heal(); as xterm does), and the second half is never anything
 * but the one that follows its first.
 *
 * What cannot be told in this format -- a character outside the tables
 * (which the font cannot draw either: it shows as a box), a wide one outside
 * them, the 129th style that a cleanup of the table (grid_gc) does not
 * free -- widens the grid first (grid_widen), so that every Cell can be
 * stored.  A wide grid is made small again by grid_compact() when it is
 * blank, or after a resize, if what is in it fits.
 *
 * The decoded rows that vt_row() hands out live in two buffers shared by all
 * screens: the program is single-threaded and uses a row between two calls.
 */
#if VT_CELL_COMPACT

#include "../_font/charset.h"
#include "../_font/emoji_charset.h"

#define WMARK 511       /* the glyph number of the first half of a wide character */

_Static_assert(CHARSET_COUNT <= WMARK && CHARSET_WIDE_COUNT <= 256 && EMOJI_COUNT <= 255,
               "the font's glyph set does not fit the glyph numbers of a small cell");

typedef struct CTab {
    uint32_t st[128];         /* fg | bg << 8 | attrs << 16 | flags << 24 */
    uint8_t  nst, hint;       /* entries used; the one found last         */
} CTab;

#define ST_FLAGS    (CELL_FG_DFL | CELL_BG_DFL | CELL_PAD)
#define ST_DEFAULT  ((uint32_t)(CELL_FG_DFL | CELL_BG_DFL) << 24)

static Cell *vt_scr[2];
static int   vt_scr_cap;

static int scratch_need(int cols)
{
    if (cols <= vt_scr_cap) return 1;
    for (int i = 0; i < 2; i++) {
        Cell *b = realloc(vt_scr[i], (size_t)cols * sizeof(Cell));
        if (!b) return 0;
        vt_scr[i] = b;
    }
    vt_scr_cap = cols;
    return 1;
}

/* The glyph number of codepoint cp: of a narrow character (wide == 0), or the
 * index of a double-width one; -1 if the tables have none. */
static int glyph_of(uint32_t cp, int wide)
{
    if (!wide) {
        if (cp - CHARSET_ASCII_LO < CHARSET_ASCII_COUNT)
            return (int)(cp - CHARSET_ASCII_LO);
        if (cp - CHARSET_LATIN1_LO < CHARSET_LATIN1_COUNT)
            return CHARSET_LATIN1_BASE + (int)(cp - CHARSET_LATIN1_LO);
        if (!cp) return 0;
        for (int i = 0; i < CHARSET_EXTRA_COUNT; i++)
            if (charset_extra[i] == cp) return CHARSET_EXTRA_BASE + i;
        return -1;
    }
    for (int i = 0; i < CHARSET_WIDE_COUNT; i++)
        if (charset_wide_extra[i] == cp) return i;
    for (int i = 0; i < EMOJI_COUNT; i++)
        if (emoji_charset[i] == cp) return 256 + i;
    return -1;
}

/* The codepoint of glyph number g (a blank for a number that is none, which
 * only a damaged grid has). */
static uint32_t cp_of(unsigned g, int wide)
{
    if (wide) {
        if (g < 256) return g < CHARSET_WIDE_COUNT ? charset_wide_extra[g] : ' ';
        return g - 256 < EMOJI_COUNT ? emoji_charset[g - 256] : ' ';
    }
    if (g < CHARSET_LATIN1_BASE) return CHARSET_ASCII_LO + g;
    if (g < CHARSET_EXTRA_BASE) return CHARSET_LATIN1_LO + (g - CHARSET_LATIN1_BASE);
    return g - CHARSET_EXTRA_BASE < CHARSET_EXTRA_COUNT ? charset_extra[g - CHARSET_EXTRA_BASE] : ' ';
}

/* The part of a cell that a style number stands for. */
static uint32_t skey(const Cell *x)
{
    unsigned f = x->flags;
    return ((f & CELL_FG_DFL) ? 0u : x->fg)
         | ((f & CELL_BG_DFL) ? 0u : (uint32_t)x->bg << 8)
         | (uint32_t)x->attrs << 16
         | (uint32_t)(f & ST_FLAGS) << 24;
}

/* The cell(s) at x (avail of them from there on) as small cells in out[0..1],
 * adding the style to the tables.  Returns the number of cells that went
 * into it (1, or 2 for a wide character and its second half); 0 if they do
 * not fit the format for what is in them (the tables have no glyph for the
 * character, or this is half a character); ENC_FULL if the style table is
 * full. */
#define ENC_FULL (-1)
static int enc16(CTab *t, const Cell *x, int avail, uint16_t *out)
{
    unsigned f = x->flags;
    int wide = (f & CELL_WIDE) != 0, g = WMARK, w = 0;
    if (f & CELL_WIDE_CONT) return 0;           /* a second half, but not after a first */
    if (wide) {
        if (avail < 2 || !(x[1].flags & CELL_WIDE_CONT) || x[1].ch ||
            skey(x) != skey(x + 1) || (w = glyph_of(x->ch, 1)) < 0)
            return 0;
    } else if ((g = glyph_of(x->ch, 0)) < 0) {
        return 0;
    }
    uint32_t key = skey(x);
    unsigned j = t->hint;
    if (t->st[j] != key) {
        for (j = 0; j < t->nst && t->st[j] != key; j++) ;
        if (j == t->nst) {
            if (j == 128) return ENC_FULL;
            t->st[t->nst++] = key;
        }
        t->hint = (uint8_t)j;
    }
    out[0] = (uint16_t)(g | j << 9);
    if (wide) out[1] = (uint16_t)w;
    return 1 + wide;
}

/* Cell number j of the style table t's style number, without its character. */
static Cell dec_style(const CTab *t, unsigned j)
{
    uint32_t k = t->st[j];
    Cell x;
    x.ch    = 0;
    x.fg    = (uint8_t)k;
    x.bg    = (uint8_t)(k >> 8);
    x.attrs = (uint8_t)(k >> 16);
    x.flags = (uint8_t)(k >> 24);
    return x;
}

/* The two halves of the wide character that starts at cell v, whose second
 * half is raw. */
static void dec_wide(const CTab *t, uint16_t v, uint16_t raw, Cell *two)
{
    two[0] = dec_style(t, v >> 9);
    two[1] = two[0];
    two[0].ch = cp_of(raw, 1);
    two[0].flags |= CELL_WIDE;
    two[1].flags |= CELL_WIDE_CONT;
}

#endif /* VT_CELL_COMPACT */

/* A new grid, blank: small if allowed, else (or if there is no memory for
 * that) full-size.  0 if there is no memory at all.  A small grid is one
 * allocation: the row pointers, the cells, the tables. */
static int grid_new(Grid *g, int rows, int cols)
{
    memset(g, 0, sizeof *g);
#if VT_CELL_COMPACT
    if (vt_compact && scratch_need(cols)) {
        size_t cells = ((size_t)rows * cols * 2 + 3) & ~(size_t)3;
        void **r = calloc(1, (size_t)rows * sizeof(void *) + cells + sizeof(CTab));
        if (r) {
            uint16_t *d = (uint16_t *)(r + rows);
            for (int i = 0; i < rows; i++) r[i] = d + (size_t)i * cols;
            g->t = (CTab *)((char *)d + cells);
            g->t->st[0] = ST_DEFAULT;
            g->t->nst = 1;
            g->row = r;
            return 1;
        }
    }
#endif
    g->row = (void **)alloc_cells(rows, cols);
    return g->row != NULL;
}

static void grid_free(Grid *g, int rows, int cols)
{
    if (g->row) {
        cells_dontneed(g, rows, cols);
        free(g->row);               /* and the tables, if any: they are in it */
    }
    memset(g, 0, sizeof *g);
}

/* Row rowp of grid g, as Cell[cols] in out. */
static void rowdec(const Grid *g, const void *rowp, int cols, Cell *out)
{
#if VT_CELL_COMPACT
    if (g->t) {
        const uint16_t *p = rowp;
        for (int c = 0; c < cols; c++) {
            uint16_t v = p[c];
            if ((v & 511) == WMARK && c + 1 < cols) {
                dec_wide(g->t, v, p[c + 1], out + c);
                c++;
                continue;
            }
            Cell x = dec_style(g->t, v >> 9);
            x.ch = (v & 511) == WMARK ? ' ' : cp_of(v & 511, 0);   /* (a 511 with no room for its half) */
            out[c] = x;
        }
        return;
    }
#endif
    memcpy(out, rowp, (size_t)cols * sizeof(Cell));
}

static const Cell *grow(const Screen *s, const Grid *g, int r, int slot)
{
#if VT_CELL_COMPACT
    if (g->t) {
        rowdec(g, g->row[r], s->cols, vt_scr[slot]);
        return vt_scr[slot];
    }
#endif
    return g->row[r];
}

const Cell *vt_row(const Screen *s, int r, int slot)
{
    return grow(s, &s->g, r, slot);
}

Cell vt_cell(const Screen *s, int r, int c)
{
#if VT_CELL_COMPACT
    if (s->g.t) {
        const uint16_t *p = s->g.row[r];
        Cell two[2];
        if (c > 0 && (p[c - 1] & 511) == WMARK) {       /* the second half */
            dec_wide(s->g.t, p[c - 1], p[c], two);
            return two[1];
        }
        if ((p[c] & 511) == WMARK && c + 1 < s->cols) {
            dec_wide(s->g.t, p[c], p[c + 1], two);
            return two[0];
        }
        Cell x = dec_style(s->g.t, p[c] >> 9);
        x.ch = cp_of(p[c] & 511, 0);
        return x;
    }
#endif
    return ((const Cell *)s->g.row[r])[c];
}

/* Make grid g (a screen's, of its size) full-size.  0 if out of memory, and
 * g as it was. */
static int grid_widen(const Screen *s, Grid *g)
{
#if VT_CELL_COMPACT
    if (!g->t) return 1;
    Cell **w = alloc_cells(s->rows, s->cols);
    if (!w) return 0;
    for (int r = 0; r < s->rows; r++) rowdec(g, g->row[r], s->cols, w[r]);
    grid_free(g, s->rows, s->cols);
    g->row  = (void **)w;
#else
    (void)s; (void)g;
#endif
    return 1;
}

#if VT_CELL_COMPACT
/* All of a row of cells as small cells in out (NULL: only see whether they
 * would fit).  1, or what enc16 says. */
static int row_enc(CTab *t, const Cell *x, int cols, uint16_t *out)
{
    uint16_t o[2];
    for (int c = 0; c < cols; ) {
        int k = enc16(t, x + c, cols - c, o);
        if (k <= 0) return k;
        if (out) { out[c] = o[0]; if (k == 2) out[c + 1] = o[1]; }
        c += k;
    }
    return 1;
}
#endif

/* Make full-size grid g small again if all of it fits. */
static void grid_compact(const Screen *s, Grid *g)
{
#if VT_CELL_COMPACT
    if (g->t || !g->row || !vt_compact) return;
    Grid n;
    if (!grid_new(&n, s->rows, s->cols)) return;
    int ok = n.t != NULL;
    for (int r = 0; ok && r < s->rows; r++)
        ok = row_enc(n.t, g->row[r], s->cols, NULL) == 1 &&
             row_enc(n.t, g->row[r], s->cols, n.row[r]) == 1;
    if (!ok) { grid_free(&n, s->rows, s->cols); return; }
    grid_free(g, s->rows, s->cols);
    *g = n;
#else
    (void)s; (void)g;
#endif
}

#if VT_CELL_COMPACT
/* The style table of small grid g (a screen's) is full: renumber the styles
 * that its cells use, dropping the rest, which are all that a long-lived
 * window collects.  Cells are rewritten in place (rows do not move).  Returns
 * 0 if every style is in use (nothing was freed). */
static int grid_gc(const Screen *s, Grid *g)
{
    CTab *t = g->t;
    uint8_t used[128] = {1};                /* style 0 is the blank */
    for (int r = 0; r < s->rows; r++) {
        const uint16_t *p = g->row[r];
        for (int c = 0; c < s->cols; c++) {
            used[p[c] >> 9] = 1;
            if ((p[c] & 511) == WMARK) c++;     /* the character, not a cell */
        }
    }
    uint8_t map[128];
    unsigned n = 0;
    for (unsigned i = 0; i < t->nst; i++)
        if (used[i]) { map[i] = (uint8_t)n; t->st[n++] = t->st[i]; }
    if (n == t->nst) return 0;
    for (int r = 0; r < s->rows; r++) {
        uint16_t *p = g->row[r];
        for (int c = 0; c < s->cols; c++) {
            int w = (p[c] & 511) == WMARK;
            p[c] = (uint16_t)((p[c] & 511) | map[p[c] >> 9] << 9);
            if (w) c++;
        }
    }
    t->nst = (uint8_t)n;
    t->hint = 0;
    return 1;
}

/* Make small grid g full-size because of what is being written to it.  Each
 * time that happens soon after the grid was made small again, an ED 2 has to
 * wait longer before it does that (cleared): a program that clears and repaints
 * wide text every frame would otherwise flip the grid every other frame. */
static int widen_for_content(Screen *s, Grid *g)
{
    if (!g->t) return 1;
    if (!grid_widen(s, g)) return 0;
    s->hold = s->flips ? (uint8_t)(1u << s->flips) : 0;
    if (s->flips < 4) s->flips++;
    return 1;
}
#endif

/* Row r of grid g (a screen's) is to become the cells x, Cell[cols]: it
 * is stored small if they fit, tidying the style table once if it is full;
 * else the grid is widened first. */
static void row_store(Screen *s, Grid *g, int r, const Cell *x)
{
#if VT_CELL_COMPACT
    if (g->t) {
        int e = row_enc(g->t, x, s->cols, NULL);
        if (e == ENC_FULL && grid_gc(s, g)) e = row_enc(g->t, x, s->cols, NULL);
        if (e == 1) { row_enc(g->t, x, s->cols, g->row[r]); return; }
        if (!widen_for_content(s, g)) { memset(g->row[r], 0, (size_t)s->cols * 2); return; }
    }
#endif
    memcpy(g->row[r], x, (size_t)s->cols * sizeof(Cell));
}

/* Is cell c of row r of g the first half of a wide character; the second? */
static int is_first(const Grid *g, int r, int c)
{
#if VT_CELL_COMPACT
    if (g->t) return (((const uint16_t *)g->row[r])[c] & 511) == WMARK;
#endif
    return (((const Cell *)g->row[r])[c].flags & CELL_WIDE) != 0;
}

static int is_second(const Grid *g, int r, int c)
{
    if (c == 0) return 0;
#if VT_CELL_COMPACT
    if (g->t) return (((const uint16_t *)g->row[r])[c - 1] & 511) == WMARK;
#endif
    return (((const Cell *)g->row[r])[c].flags & CELL_WIDE_CONT) != 0;
}

/* Store the narrow cell x at (r, c) of grid g, which is a screen's; the grid
 * is widened if x does not fit.  Not a part of a wide character: see cut(). */
static void store1(Screen *s, Grid *g, int r, int c, const Cell *x)
{
#if VT_CELL_COMPACT
    if (g->t) {
        uint16_t v[2];
        int e = enc16(g->t, x, 1, v);
        if (e == ENC_FULL && grid_gc(s, g)) e = enc16(g->t, x, 1, v);
        if (e == 1) { ((uint16_t *)g->row[r])[c] = v[0]; return; }
        if (!widen_for_content(s, g)) { ((uint16_t *)g->row[r])[c] = 0; return; }
    }
#endif
    ((Cell *)g->row[r])[c] = *x;
}

/* Columns a..b of row r of g are about to be overwritten.  A wide character
 * that this cuts in two (it starts before a, or ends after b) loses its other
 * half: it becomes a blank, as in xterm. */
static void cut(Screen *s, Grid *g, int r, int a, int b)
{
    Cell bl = blank_cell(s);
    int first = a > 0 && is_second(g, r, a);
    int last  = b + 1 < s->cols && is_first(g, r, b);
    /* The first half goes first, so that what is left in between (the second half
     * as a cell of its own) is still a grid that can be widened. */
    if (first) store1(s, g, r, a - 1, &bl);
    if (last) { store1(s, g, r, b, &bl); store1(s, g, r, b + 1, &bl); }
}

/* Store x at (r, c) of grid g (a screen's); a narrow cell. */
static void cput(Screen *s, Grid *g, int r, int c, Cell x)
{
    cut(s, g, r, c, c);
    store1(s, g, r, c, &x);
}

/* Store the wide character x (x has the CELL_WIDE flag) and its second half,
 * at (r, c) and (r, c + 1). */
static void cput_wide(Screen *s, Grid *g, int r, int c, Cell x)
{
    cut(s, g, r, c, c + 1);
#if VT_CELL_COMPACT
    if (g->t) {
        Cell two[2] = { x, x };
        two[1].ch = 0;
        two[1].flags = (uint8_t)((x.flags & ~CELL_WIDE) | CELL_WIDE_CONT);
        uint16_t v[2];
        int e = enc16(g->t, two, 2, v);
        if (e == ENC_FULL && grid_gc(s, g)) e = enc16(g->t, two, 2, v);
        if (e == 2) { ((uint16_t *)g->row[r])[c] = v[0]; ((uint16_t *)g->row[r])[c + 1] = v[1]; return; }
        if (!widen_for_content(s, g)) { ((uint16_t *)g->row[r])[c] = ((uint16_t *)g->row[r])[c + 1] = 0; return; }
    }
#endif
    Cell *row = g->row[r];
    row[c] = x;
    row[c + 1] = x;
    row[c + 1].ch = 0;
    row[c + 1].flags = (uint8_t)((x.flags & ~CELL_WIDE) | CELL_WIDE_CONT);
}

/* Make the blank cell, as the current rendition draws it, storable in the
 * screen being shown, widening it if it is not: the operations that move rows
 * about and blank some of them call this first, so that none of them can find
 * the grid replaced half way. */
static void blank_ready(Screen *s)
{
#if VT_CELL_COMPACT
    if (!s->g.t) return;
    Cell b = blank_cell(s);
    uint16_t v[2];
    int e = enc16(s->g.t, &b, 1, v);
    if (e == ENC_FULL && grid_gc(s, &s->g)) e = enc16(s->g.t, &b, 1, v);
    if (e != 1) widen_for_content(s, &s->g);
#else
    (void)s;
#endif
}

/* Erase columns from..to of row r of the screen being shown. */
static void fill(Screen *s, int r, int from, int to)
{
    Cell b = blank_cell(s);
    Grid *g = &s->g;
    cut(s, g, r, from, to);
#if VT_CELL_COMPACT
    if (g->t) {
        uint16_t v[2];
        int e = enc16(g->t, &b, 1, v);
        if (e == ENC_FULL && grid_gc(s, g)) e = enc16(g->t, &b, 1, v);
        if (e == 1) {
            for (int c = from; c <= to; c++) ((uint16_t *)g->row[r])[c] = v[0];
            return;
        }
        if (!widen_for_content(s, g)) return;
    }
#endif
    for (int c = from; c <= to; c++) ((Cell *)g->row[r])[c] = b;
}

/* Erase the whole screen being shown, which is a new grid. */
static void fill_new(Screen *s)
{
#if VT_CELL_COMPACT
    if (s->g.t) {
        Cell b = blank_cell(s);
        uint16_t v[2];
        int e = enc16(s->g.t, &b, 1, v);
        if (e == ENC_FULL && grid_gc(s, &s->g)) e = enc16(s->g.t, &b, 1, v);
        if (e == 1 && !v[0]) return;                /* it is zero already */
    }
#endif
    for (int r = 0; r < s->rows; r++) fill(s, r, 0, s->cols - 1);
}

/* A wide character in x[c] that is only half of one (a first half with no
 * second, a second with no first) becomes a blank. */
static void heal(Cell *x, int c, int cols, Cell blank)
{
    if (c < 0 || c >= cols) return;
    if (((x[c].flags & CELL_WIDE_CONT) && (c == 0 || !(x[c - 1].flags & CELL_WIDE))) ||
        ((x[c].flags & CELL_WIDE) && (c + 1 >= cols || !(x[c + 1].flags & CELL_WIDE_CONT))))
        x[c] = blank;
}

/* Move n cells of row r of the screen being shown from column src to dst.  The
 * row is worked on as full-size cells, so that a wide character that this
 * leaves in half can be blanked at the places where it can happen. */
static void row_move(Screen *s, int r, int dst, int src, int n)
{
    Cell *x = s->g.row[r];
#if VT_CELL_COMPACT
    if (s->g.t) { x = vt_scr[1]; rowdec(&s->g, s->g.row[r], s->cols, x); }
#endif
    memmove(x + dst, x + src, (size_t)n * sizeof(Cell));
    Cell b = blank_cell(s);
    heal(x, dst - 1, s->cols, b); heal(x, dst, s->cols, b);
    heal(x, dst + n - 1, s->cols, b); heal(x, dst + n, s->cols, b);
#if VT_CELL_COMPACT
    if (s->g.t) row_store(s, &s->g, r, x);
#endif
}

/* The screen has just been cleared (ED 2/3): a grid that went full-size may
 * be small again.  ED 2 is also what a program repainting wide text sends
 * every frame, so that one waits out some clears (hold) after the widening;
 * how many grows with every flip (widen_for_content), and ED 3, which is a
 * program asking to forget, makes it none. */
static void cleared(Screen *s, int force)
{
    if (force) s->hold = s->flips = 0;
    if (!GT(&s->g) && s->hold) s->hold--;
    else grid_compact(s, &s->g);
}

/* ------------------------------------------------------------------ */
/* Tab stops                                                            */
/* ------------------------------------------------------------------ */

static void tabs_reset(VTParser *p)
{
    memset(p->tabstops, 0, sizeof(p->tabstops));
    for (int c = 0; c < p->scr.cols && c < VT_MAX_COLS; c += 8)
        p->tabstops[c / 8] |= (uint8_t)(1u << (c % 8));
}

static bool tab_get(const VTParser *p, int col)
{
    if (col < 0 || col >= VT_MAX_COLS) return false;
    return (p->tabstops[col / 8] >> (col % 8)) & 1;
}

static void tab_set(VTParser *p, int col)
{
    if (col >= 0 && col < VT_MAX_COLS)
        p->tabstops[col / 8] |= (uint8_t)(1u << (col % 8));
}

static void tab_clear(VTParser *p, int col)
{
    if (col >= 0 && col < VT_MAX_COLS)
        p->tabstops[col / 8] &= (uint8_t)~(1u << (col % 8));
}

/* ================================================================== */
/* Screen operations                                                    */
/* ================================================================== */

static void scr_init(Screen *s, int rows, int cols, bool history)
{
    memset(s, 0, sizeof(*s));
    s->rows           = rows;
    s->cols           = cols;
    grid_new(&s->g, rows, cols);
    /* s->ag: allocated lazily on first ?1047h/?1049h */
    s->row_flags = calloc((size_t)rows, 1);
#if SCROLLBACK_ENABLED
    s->scrollback     = history ? calloc(1, SCROLLBACK_BYTES) : NULL;
    s->scrollback_len = 0;
#else
    (void)history;
#endif
    s->cur_visible    = true;
    s->auto_wrap      = true;
    s->scroll_top     = 0;
    s->scroll_bottom  = rows - 1;
    s->cur_fg         = COLOR_DEFAULT;
    s->cur_bg         = COLOR_DEFAULT;
}

/* The alternate screen is a second grid with its own wrap flags, kept
 * apart from the primary's: entering it swaps both in and clears the new
 * pair, leaving swaps them back. */
static void alt_swap(Screen *s)
{
    Grid g = s->g;           s->g         = s->ag;            s->ag            = g;
    uint8_t *f = s->row_flags; s->row_flags = s->alt_row_flags; s->alt_row_flags = f;
    s->in_alt_screen = !s->in_alt_screen;
}

/* Returns 0, and stays on the primary screen, if there is no memory for the
 * alternate one. */
static int alt_enter(Screen *s)
{
    if (!s->ag.row && !grid_new(&s->ag, s->rows, s->cols)) return 0;
    if (!s->alt_row_flags)  s->alt_row_flags  = calloc((size_t)s->rows, 1);
    if (!s->alt_row_flags) return 0;
    alt_swap(s);
    memset(s->row_flags, 0, (size_t)s->rows);
    return 1;
}

/* Free the grid in ag (after alt_swap that is the alternate screen's,
 * once it is left).  The flags stay: they are one byte a row. */
static void alt_release(Screen *s)
{
    grid_free(&s->ag, s->rows, s->cols);
}

static void scr_free(Screen *s)
{
    free(s->alt_row_flags);
    s->alt_row_flags = NULL;
    free(s->row_flags);
    s->row_flags = NULL;
#if SCROLLBACK_ENABLED
    free(s->scrollback);
    s->scrollback = NULL;
#endif
    /* Hint to the kernel: these pages will not be read again.
     * This immediately reduces RSS for the calling process even though
     * the virtual address range stays committed to the allocator. */
    grid_free(&s->g,  s->rows, s->cols);
    grid_free(&s->ag, s->rows, s->cols);
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                            */
/* ------------------------------------------------------------------ */

/*
 * Scroll the scroll region UP by `n` lines.
 * Lines at scroll_top..scroll_top+n-1 are discarded;
 * scroll_top+n..scroll_bottom shift up; n blank lines appear at bottom.
 */

#if SCROLLBACK_ENABLED
/*
 * Encode one Cell row as plain UTF-8 into the scrollback buffer.
 * Only called when scroll_top==0 (full-screen scroll) and the primary
 * screen is active, so we don't capture app-internal scroll regions or
 * alternate-screen output.
 *
 * Algorithm:
 *  - Convert each cell's codepoint to UTF-8 (skip wide continuations)
 *  - Stop at first cell from the right that is non-space (trim trailing ws)
 *  - Append '\n'
 *  - If the buffer has no room, drop the oldest line (first '\n') first
 */
static void scrollback_capture(Screen *s, const void *rowp, uint8_t rflags)
{
    if (!s->scrollback) return;

    const Cell *row = rowp;
#if VT_CELL_COMPACT
    if (s->g.t) { rowdec(&s->g, rowp, s->cols, vt_scr[0]); row = vt_scr[0]; }
#endif

    /* Encode row to a temp buffer */
    char tmp[4096];
    int  tlen = 0;
    int  last_nonspace = -1;

    for (int c = 0; c < s->cols && tlen < (int)sizeof(tmp) - 4; c++) {
        const Cell *cell = &row[c];
        if (cell->flags & (CELL_WIDE_CONT | CELL_PAD)) continue;

        uint32_t cp = cell->ch ? cell->ch : ' ';
        /* Encode UTF-8 */
        if (cp < 0x80) {
            tmp[tlen++] = (char)cp;
        } else if (cp < 0x800) {
            tmp[tlen++] = (char)(0xC0 | (cp >> 6));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            tmp[tlen++] = (char)(0xE0 | (cp >> 12));
            tmp[tlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        } else {
            tmp[tlen++] = (char)(0xF0 | (cp >> 18));
            tmp[tlen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            tmp[tlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            tmp[tlen++] = (char)(0x80 | (cp & 0x3F));
        }
        if (cp != ' ' && cp != 0) last_nonspace = tlen;
    }

    /* Trim trailing whitespace.
     * If the row was autowrapped (ROW_WRAPPED), don't append '\n': the
     * next captured row continues this logical line.  The buffer's last
     * byte implicitly encodes the pending-wrap state (no '\n' = open line).
     * When the buffer is full we must always append '\n' first so the eviction
     * loop can find a complete line boundary to drop. */
    if (rflags & ROW_WRAPPED) {
        /* Keep trailing spaces on wrapped rows — they pad to the full
         * width and the next row's content must start in the right column */
        tlen = tlen;   /* no trim: spaces are significant for column alignment */
    } else {
        tlen = last_nonspace >= 0 ? last_nonspace : 0;
        tmp[tlen++] = '\n';
    }

    /* Make room: drop oldest line(s) until tlen bytes fit */
    while (s->scrollback_len + tlen > SCROLLBACK_BYTES) {
        char *nl = memchr(s->scrollback, '\n', (size_t)s->scrollback_len);
        if (!nl) { s->scrollback_len = 0; break; }   /* shouldn't happen */
        int drop = (int)(nl - s->scrollback) + 1;
        memmove(s->scrollback, s->scrollback + drop,
                (size_t)(s->scrollback_len - drop));
        s->scrollback_len -= drop;
    }

    /* Append */
    memcpy(s->scrollback + s->scrollback_len, tmp, (size_t)tlen);
    s->scrollback_len += tlen;
}
#endif /* SCROLLBACK_ENABLED */

/* Rotate rows top..top+height-1 (of the screen being shown, and their wrap
 * flags) up by k: the first k go to the end, the rest move up.  Only the row
 * pointers move, not the cells; no memory is needed. */
static void rot_rows(Screen *s, int top, int height, int k)
{
    for (int pass = 0; pass < 3; pass++) {
        int lo = pass == 1 ? k : 0, hi = pass == 0 ? k : height;
        for (int i = lo, j = hi - 1; i < j; i++, j--) {
            void *t = s->g.row[top + i]; s->g.row[top + i] = s->g.row[top + j]; s->g.row[top + j] = t;
            if (s->row_flags) {
                uint8_t f = s->row_flags[top + i];
                s->row_flags[top + i] = s->row_flags[top + j]; s->row_flags[top + j] = f;
            }
        }
    }
}

static void scroll_up(Screen *s, int n)
{
    if (n <= 0) return;
    int height = s->scroll_bottom - s->scroll_top + 1;
    if (n > height) n = height;
    blank_ready(s);

#if SCROLLBACK_ENABLED
    /* Capture departing lines into scrollback.
     * Only for full-screen scrolls (scroll_top==0) on the primary screen:
     * - apps with internal scroll regions (vim, less) manage their own
     *   viewport and their lines should not appear in terminal history;
     * - alternate screen content is temporary and not historically useful. */
    if (s->scroll_top == 0 && !s->in_alt_screen) {
        for (int i = 0; i < n; i++)
            scrollback_capture(s, s->g.row[i], s->row_flags ? s->row_flags[i] : 0);
    }
#endif

    /* The first n rows (the ones that go) are recycled as the last n, blank */
    rot_rows(s, s->scroll_top, height, n);
    for (int i = s->scroll_bottom - n + 1; i <= s->scroll_bottom; i++) {
        fill(s, i, 0, s->cols - 1);
        if (s->row_flags) s->row_flags[i] = 0;
    }
}

/*
 * Scroll the scroll region DOWN by `n` lines.
 * Lines at scroll_bottom-n+1..scroll_bottom are discarded;
 * scroll_top..scroll_bottom-n shift down; n blank lines at top.
 * The wrap flag belongs to its line: it moves down with it, and the
 * blank lines put in at the top have none.
 */
static void scroll_down(Screen *s, int n)
{
    if (n <= 0) return;
    int height = s->scroll_bottom - s->scroll_top + 1;
    if (n > height) n = height;
    blank_ready(s);

    rot_rows(s, s->scroll_top, height, height - n);
    for (int i = s->scroll_top; i < s->scroll_top + n; i++) {
        fill(s, i, 0, s->cols - 1);
        if (s->row_flags) s->row_flags[i] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Cursor movement                                                      */
/* ------------------------------------------------------------------ */

static void cursor_set(Screen *s, int row, int col)
{
    int min_row = s->origin_mode ? s->scroll_top    : 0;
    int max_row = s->origin_mode ? s->scroll_bottom : s->rows - 1;

    if (row < min_row) row = min_row;
    if (row > max_row) row = max_row;
    if (col < 0)       col = 0;
    if (col >= s->cols) col = s->cols - 1;

    s->cur_row    = row;
    s->cur_col    = col;
    s->pending_wrap = false;
}

/* ------------------------------------------------------------------ */
/* LF / reverse-index helpers (used by both C0 and CSI handlers)       */
/* ------------------------------------------------------------------ */

static void do_lf(Screen *s)
{
    if (s->cur_row == s->scroll_bottom)
        scroll_up(s, 1);
    else if (s->cur_row < s->rows - 1)
        s->cur_row++;
    s->pending_wrap = false;
}

static void do_ri(Screen *s)   /* reverse index */
{
    if (s->cur_row == s->scroll_top)
        scroll_down(s, 1);
    else if (s->cur_row > 0)
        s->cur_row--;
    s->pending_wrap = false;
}

/* ================================================================== */
/* Character output                                                     */
/* ================================================================== */

/* Columns a character takes: 2 for a wide one, 1 for most, 0 for the
 * zero-width ones.  Width comes from our own tables, not libc's wcwidth(),
 * which depends on the locale.  Zero-width (ZWJ, variation selectors, ...)
 * must take no cell: wcwidth() says 0, and readline believes it, so taking
 * a cell for one would put the screen out of step with the line being
 * edited. */
static int cp_width(uint32_t ch)
{
    if (ch == 0x200D || (ch >= 0xFE00 && ch <= 0xFE0F) || (ch >= 0x200B && ch <= 0x200F))
        return 0;
    return (ch >= 0x1100 && (emoji_wide(ch) || cjk_wide(ch))) ? 2 : 1;
}

/* DEC special graphics, 0x60..0x7e, as Unicode. */
static const uint16_t dec_graphics[31] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
    0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd,
    0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3,
    0x00b7
};

static void put_char(VTParser *p, uint32_t ch)
{
    Screen *s = &p->scr;

    /* Width comes from our own compile-time emoji table, not libc's
     * wcwidth(): that one is locale-dependent, returning -1 (silently
     * discarding the character right here) for anything outside ASCII
     * whenever no UTF-8 locale is configured -- confirmed to misbehave
     * that way in practice, and not a safe assumption for a minimal
     * target system. The emoji blocks and the
     * fullwidth forms and CJK (see width.h) are double-width whether or
     * not we can draw that particular one; everything else is 1. Whether
     * a codepoint can actually be drawn is decided separately, at render
     * time. The >= 0x1100 guard keeps the table scans off the hot path
     * for all ordinary text. */
    int width = cp_width(ch);
    if (!width || (width == 2 && s->cols < 2)) return;     /* (no room for a wide one at all) */
    p->last_graphic = ch;   /* what REP repeats */

    /* Handle pending wrap: wrap to next line before placing char */
    if (s->pending_wrap) {
        if (s->auto_wrap) {
            if (s->row_flags) s->row_flags[s->cur_row] |= ROW_WRAPPED;
            s->cur_col    = 0;
            s->pending_wrap = false;
            do_lf(s);
        } else {
            s->pending_wrap = false;
        }
    }

    /* If a double-wide char doesn't fit, pad with space and wrap */
    if (width == 2 && s->cur_col + 1 >= s->cols) {
        Cell pad = blank_cell(s);
        pad.flags |= CELL_PAD;
        cput(s, &s->g, s->cur_row, s->cur_col, pad);
        if (s->auto_wrap) {
            if (s->row_flags) s->row_flags[s->cur_row] |= ROW_WRAPPED;
            s->cur_col = 0;
            do_lf(s);
        } else {
            return; /* no room and no wrap – discard */
        }
    }

    /* Insert mode: shift line content right */
    if (s->insert_mode) {
        int tail = s->cols - s->cur_col - width;
        if (tail > 0)
            row_move(s, s->cur_row, s->cur_col + width, s->cur_col, tail);
        /* blank the inserted slot(s) */
        fill(s, s->cur_row, s->cur_col, s->cur_col + width - 1);
    }

    /* Write the character: one cell, or a wide one's first and second halves */
    Cell cell = { .ch = ch, .attrs = s->cur_attrs };
    if (s->cur_fg == COLOR_DEFAULT) cell.flags |= CELL_FG_DFL;
    else                             cell.fg     = (uint8_t)s->cur_fg;
    if (s->cur_bg == COLOR_DEFAULT) cell.flags |= CELL_BG_DFL;
    else                             cell.bg     = (uint8_t)s->cur_bg;
    if (width == 2)                  cell.flags |= CELL_WIDE;
    if (width == 2) cput_wide(s, &s->g, s->cur_row, s->cur_col, cell);   /* (it fits: see above) */
    else            cput(s, &s->g, s->cur_row, s->cur_col, cell);

    /* Advance cursor */
    int new_col = s->cur_col + width;
    if (new_col >= s->cols) {
        s->cur_col    = s->cols - 1;
        s->pending_wrap = true;     /* the wrap itself, and its flag, come with the next char */
    } else {
        s->cur_col    = new_col;
        s->pending_wrap = false;
    }
}

/* ================================================================== */
/* C0 controls                                                          */
/* ================================================================== */

static void handle_c0(VTParser *p, uint8_t c)
{
    Screen *s = &p->scr;
    switch (c) {
    case 0x07: /* BEL: reported to the caller, see VTParser.bell */
        p->bell = true;
        break;
    case 0x08: /* BS */
        if (s->cur_col > 0) { s->cur_col--; s->pending_wrap = false; }
        break;
    case 0x09: /* HT */
        if (s->cur_col < s->cols - 1) {
            do {
                s->cur_col++;
            } while (s->cur_col < s->cols - 1 && !tab_get(p, s->cur_col));
        }
        s->pending_wrap = false;
        break;
    case 0x0A: case 0x0B: case 0x0C: /* LF / VT / FF */
        do_lf(s);
        break;
    case 0x0D: /* CR */
        s->cur_col    = 0;
        s->pending_wrap = false;
        break;
    case 0x0E: case 0x0F: /* SO / SI – charset switching, ignore */
        break;
    }
}

/* ================================================================== */
/* SGR handler                                                          */
/* ================================================================== */

static void handle_sgr(VTParser *p)
{
    Screen *s = &p->scr;

    if (p->num_params == 0) {
        s->cur_fg    = COLOR_DEFAULT;
        s->cur_bg    = COLOR_DEFAULT;
        s->cur_attrs = 0;
        return;
    }

    int i = 0;
    while (i < p->num_params) {
        int v = p->params[i];
        switch (v) {
        case 0:
            s->cur_fg    = COLOR_DEFAULT;
            s->cur_bg    = COLOR_DEFAULT;
            s->cur_attrs = 0;
            break;
        case 1: s->cur_attrs |=  ATTR_BOLD;      break;
        case 2: s->cur_attrs |=  ATTR_DIM;       break;
        case 3: s->cur_attrs |=  ATTR_ITALIC;    break;
        case 4: s->cur_attrs |=  ATTR_UNDERLINE; break;
        case 5: s->cur_attrs |=  ATTR_BLINK;     break;
        case 7: s->cur_attrs |=  ATTR_REVERSE;   break;
        case 8: s->cur_attrs |=  ATTR_INVIS;     break;
        case 9: s->cur_attrs |=  ATTR_STRIKE;    break;
        case 22: s->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: s->cur_attrs &= ~ATTR_ITALIC;    break;
        case 24: s->cur_attrs &= ~ATTR_UNDERLINE; break;
        case 25: s->cur_attrs &= ~ATTR_BLINK;     break;
        case 27: s->cur_attrs &= ~ATTR_REVERSE;   break;
        case 28: s->cur_attrs &= ~ATTR_INVIS;     break;
        case 29: s->cur_attrs &= ~ATTR_STRIKE;    break;
        case 39: s->cur_fg = COLOR_DEFAULT; break;
        case 49: s->cur_bg = COLOR_DEFAULT; break;
        /* 8-colour fg */
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            s->cur_fg = (uint16_t)(v - 30); break;
        /* 8-colour bg */
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            s->cur_bg = (uint16_t)(v - 40); break;
        /* bright fg */
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            s->cur_fg = (uint16_t)(v - 90 + 8); break;
        /* bright bg */
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            s->cur_bg = (uint16_t)(v - 100 + 8); break;
        /* 256-colour fg */
        case 38:
            if (i + 2 < p->num_params && p->params[i + 1] == 5) {
                if (p->params[i + 2] <= 255)     /* else not a colour: ignored */
                    s->cur_fg = (uint16_t)p->params[i + 2];
                i += 2;
            } else if (i + 4 < p->num_params && p->params[i + 1] == 2) {
                /* truecolor: approximate as 256-colour – TODO later */
                i += 4;
            }
            break;
        /* 256-colour bg */
        case 48:
            if (i + 2 < p->num_params && p->params[i + 1] == 5) {
                if (p->params[i + 2] <= 255)     /* else not a colour: ignored */
                    s->cur_bg = (uint16_t)p->params[i + 2];
                i += 2;
            } else if (i + 4 < p->num_params && p->params[i + 1] == 2) {
                i += 4;
            }
            break;
        default: break;
        }
        i++;
    }
}

/* ================================================================== */
/* CSI dispatch                                                         */
/* ================================================================== */

static void dispatch_csi(VTParser *p, char final)
{
    Screen *s = &p->scr;

    if (!p->priv) {
        switch (final) {

        /* ---- cursor movement ---- */
        case 'A': cursor_set(s, s->cur_row - P(0,1), s->cur_col);  break;
        case 'B': cursor_set(s, s->cur_row + P(0,1), s->cur_col);  break;
        case 'C': cursor_set(s, s->cur_row, s->cur_col + P(0,1));  break;
        case 'D': cursor_set(s, s->cur_row, s->cur_col - P(0,1));  break;
        case 'E': cursor_set(s, s->cur_row + P(0,1), 0);           break;
        case 'F': cursor_set(s, s->cur_row - P(0,1), 0);           break;
        case 'G': cursor_set(s, s->cur_row, P(0,1) - 1);           break;
        case 'd': cursor_set(s, P(0,1) - 1, s->cur_col);           break;

        case 'H': case 'f': {
            int row = P(0,1) - 1;
            int col = P(1,1) - 1;
            if (s->origin_mode) row += s->scroll_top;
            cursor_set(s, row, col);
            break;
        }

        /* CHT – forward tab */
        case 'I':
            for (int n = P(0,1); n > 0; n--) handle_c0(p, 0x09);
            break;

        /* CBT – backward tab */
        case 'Z': {
            int n = P(0,1);
            while (n-- > 0 && s->cur_col > 0) {
                s->cur_col--;
                while (s->cur_col > 0 && !tab_get(p, s->cur_col))
                    s->cur_col--;
            }
            s->pending_wrap = false;
            break;
        }

        /* ---- erase ---- */
        case 'J': {
            int m = P(0,0);
            if (m == 0 || m == 2) {
                /* erase from cursor to end (0), or whole screen (2) */
                int start_row = (m == 2) ? 0 : s->cur_row;
                int start_col = (m == 2) ? 0 : s->cur_col;
                fill(s, start_row, start_col, s->cols - 1);
                for (int r = start_row + 1; r < s->rows; r++)
                    fill(s, r, 0, s->cols - 1);
                /* The rows from the start of the erase down lose their wrap
                 * flags (as xterm does, the row the erase starts on too). */
                if (s->row_flags)
                    memset(s->row_flags + start_row, 0, (size_t)(s->rows - start_row));
                if (m == 2) cleared(s, 0);
            } else if (m == 1) {
                /* erase from top to cursor */
                for (int r = 0; r < s->cur_row; r++)
                    fill(s, r, 0, s->cols - 1);
                fill(s, s->cur_row, 0, s->cur_col);
                if (s->row_flags) memset(s->row_flags, 0, (size_t)s->cur_row);
            } else if (m == 3) {
                /* erase scrollback – same as 2 for our purposes */
                for (int r = 0; r < s->rows; r++)
                    fill(s, r, 0, s->cols - 1);
                if (s->row_flags) memset(s->row_flags, 0, (size_t)s->rows);
                cleared(s, 1);
            }
            break;
        }
        case 'K': {
            int m = P(0,0);
            /* What xterm does with the wrap flag of the row (observed on
             * xterm with a line that wraps and is then erased): EL 0, from
             * any column, clears it; EL 2 does not. */
            if (m == 0) {
                fill(s, s->cur_row, s->cur_col, s->cols - 1);
                if (s->row_flags) s->row_flags[s->cur_row] = 0;
            } else if (m == 1) {
                fill(s, s->cur_row, 0, s->cur_col);
            } else if (m == 2) {
                fill(s, s->cur_row, 0, s->cols - 1);
            }
            break;
        }
        case 'X': { /* ECH – erase N chars */
            int n = P(0,1);
            int end = s->cur_col + n - 1;
            if (end >= s->cols) end = s->cols - 1;
            fill(s, s->cur_row, s->cur_col, end);
            break;
        }

        /* ---- repeat ---- */
        case 'b': { /* REP – repeat the last graphic char P(0,1) times,
                     * with the current rendition, as if typed again */
            int n = P(0,1);
            while (p->last_graphic && n-- > 0)
                put_char(p, p->last_graphic);
            break;
        }

        /* ---- insert / delete ---- */
        case '@': { /* ICH – insert N blank chars */
            int n = P(0,1);
            if (n > s->cols - s->cur_col) n = s->cols - s->cur_col;
            row_move(s, s->cur_row, s->cur_col + n, s->cur_col,
                     s->cols - s->cur_col - n);
            fill(s, s->cur_row, s->cur_col, s->cur_col + n - 1);
            break;
        }
        case 'P': { /* DCH – delete N chars */
            int n = P(0,1);
            if (n > s->cols - s->cur_col) n = s->cols - s->cur_col;
            row_move(s, s->cur_row, s->cur_col, s->cur_col + n,
                     s->cols - s->cur_col - n);
            fill(s, s->cur_row, s->cols - n, s->cols - 1);
            break;
        }
        case 'L': { /* IL – insert N lines */
            if (s->cur_row >= s->scroll_top && s->cur_row <= s->scroll_bottom) {
                int saved = s->scroll_top;
                s->scroll_top = s->cur_row;
                scroll_down(s, P(0,1));
                s->scroll_top = saved;
            }
            s->cur_col    = 0;
            s->pending_wrap = false;
            break;
        }
        case 'M': { /* DL – delete N lines */
            if (s->cur_row >= s->scroll_top && s->cur_row <= s->scroll_bottom) {
                int saved = s->scroll_top;
                s->scroll_top = s->cur_row;
                scroll_up(s, P(0,1));
                s->scroll_top = saved;
            }
            s->cur_col    = 0;
            s->pending_wrap = false;
            break;
        }

        /* ---- scroll ---- */
        case 'S': scroll_up  (s, P(0,1)); break;
        case 'T': scroll_down(s, P(0,1)); break;

        /* ---- attributes ---- */
        case 'm': handle_sgr(p); break;

        /* ---- modes ---- */
        case 'h':
            if (P(0,0) == 4) s->insert_mode = true;
            break;
        case 'l':
            if (P(0,0) == 4) s->insert_mode = false;
            break;

        /* ---- tab stops ---- */
        case 'g':
            if (P(0,0) == 0)
                tab_clear(p, s->cur_col);
            else if (P(0,0) == 3)
                memset(p->tabstops, 0, sizeof(p->tabstops));
            break;

        /* ---- scroll region ---- */
        case 'r': {
            int top = P(0,1) - 1;
            int bot = P(1, s->rows) - 1;
            if (top < 0) top = 0;
            if (bot >= s->rows) bot = s->rows - 1;
            if (top < bot) {
                s->scroll_top    = top;
                s->scroll_bottom = bot;
            }
            /* Cursor goes to home (origin-relative if DECOM) */
            cursor_set(s, s->origin_mode ? s->scroll_top : 0, 0);
            break;
        }

        /* ---- save / restore cursor (ANSI variants) ---- */
        case 's':
            s->saved_row   = s->cur_row;
            s->saved_col   = s->cur_col;
            s->saved_fg    = s->cur_fg;
            s->saved_bg    = s->cur_bg;
            s->saved_attrs = s->cur_attrs;
            s->saved_wrap  = s->auto_wrap;
            break;
        case 'u':
            s->cur_row    = s->saved_row;
            s->cur_col    = s->saved_col;
            s->cur_fg     = s->saved_fg;
            s->cur_bg     = s->saved_bg;
            s->cur_attrs  = s->saved_attrs;
            s->auto_wrap  = s->saved_wrap;
            s->pending_wrap = false;
            break;

        /* ---- device status ---- */
        case 'n':
            if (P(0,0) == 6)
                p->cpr_requested = true;   /* main loop will send reply */
            break;

        default: break;
        }

    } else {
        /* CSI ? ... */
        switch (final) {

        case 'h': /* DEC private set */
            for (int i = 0; i < p->num_params; i++) {
                switch (p->params[i]) {
                case 1:  s->app_cursor  = true;  break;
                case 6:  s->origin_mode = true;
                         cursor_set(s, s->scroll_top, 0); break;
                case 7:  s->auto_wrap   = true;  break;
                case 12: /* blinking cursor – ignore */  break;
                case 25: s->cur_visible = true;  break;
                case 47: case 1047: case 1049: /* alt screen (we do not tell them apart) */
                    if (!s->in_alt_screen) {
                        s->alt_saved_row   = s->cur_row;
                        s->alt_saved_col   = s->cur_col;
                        s->alt_saved_fg    = s->cur_fg;
                        s->alt_saved_bg    = s->cur_bg;
                        s->alt_saved_attrs = s->cur_attrs;
                        if (!alt_enter(s)) break;
                        /* Primary grid (now in ag) is no longer hot;
                         * let the kernel demote its pages in the LRU. */
                        cells_cold(&s->ag, s->rows, s->cols);
                        fill_new(s);
                        s->cur_row = 0; s->cur_col = 0;
                        s->scroll_top = 0; s->scroll_bottom = s->rows - 1;
                    }
                    break;
                case 1000: s->mouse_mode = 1; break;
                case 1002: s->mouse_mode = 2; break;
                case 1003: s->mouse_mode = 3; break;
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
                case 1005: s->mouse_enc  = 1; break;
#endif
                case 1006: s->mouse_enc  = 2; break;
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
                case 1015: s->mouse_enc  = 3; break;
#endif
                case 2004: s->bracketed_paste = true; break;
                default:   break;
                }
            }
            break;

        case 'l': /* DEC private reset */
            for (int i = 0; i < p->num_params; i++) {
                switch (p->params[i]) {
                case 1:  s->app_cursor  = false; break;
                case 6:  s->origin_mode = false;
                         cursor_set(s, 0, 0);    break;
                case 7:  s->auto_wrap   = false; break;
                case 12: break;
                case 25: s->cur_visible = false; break;
                case 47: case 1047: case 1049:
                    if (s->in_alt_screen) {
                        alt_swap(s);
                        /* The alt grid (now in ag) is dead: the next
                         * entry starts from a blank one, so give it back
                         * rather than keep its pages resident. */
                        alt_release(s);
                        s->cur_row  = s->alt_saved_row;
                        s->cur_col  = s->alt_saved_col;
                        s->cur_fg   = s->alt_saved_fg;
                        s->cur_bg   = s->alt_saved_bg;
                        s->cur_attrs = s->alt_saved_attrs;
                    }
                    break;
                /* As in xterm, resetting any tracking mode turns tracking
                 * off; an encoding goes back to the default only if it is
                 * the one in use. */
                case 1000: case 1002: case 1003: s->mouse_mode = 0; break;
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
                case 1005: if (s->mouse_enc == 1) s->mouse_enc = 0; break;
#endif
                case 1006: if (s->mouse_enc == 2) s->mouse_enc = 0; break;
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
                case 1015: if (s->mouse_enc == 3) s->mouse_enc = 0; break;
#endif
                case 2004: s->bracketed_paste = false; break;
                default:   break;
                }
            }
            break;

        default: break;
        }
    }
}

/* ================================================================== */
/* OSC dispatch                                                         */
/* ================================================================== */

static void dispatch_osc(VTParser *p)
{
    p->osc[p->osc_len < VT_MAX_OSC - 1 ? p->osc_len : VT_MAX_OSC - 1] = '\0';
    char *sep = strchr(p->osc, ';');
    if (!sep) return;
    int cmd = atoi(p->osc);
    if (cmd == 0 || cmd == 1 || cmd == 2)
        snprintf(p->scr.title, sizeof(p->scr.title), "%s", sep + 1);
}

/* ================================================================== */
/* ESC dispatch                                                         */
/* ================================================================== */

static void dispatch_esc(VTParser *p, char final)
{
    Screen *s = &p->scr;
    switch (final) {
    case '7': /* DECSC – save cursor */
        s->saved_row   = s->cur_row;
        s->saved_col   = s->cur_col;
        s->saved_fg    = s->cur_fg;
        s->saved_bg    = s->cur_bg;
        s->saved_attrs = s->cur_attrs;
        s->saved_wrap  = s->auto_wrap;
        break;
    case '8': /* DECRC – restore cursor */
        s->cur_row    = s->saved_row;
        s->cur_col    = s->saved_col;
        s->cur_fg     = s->saved_fg;
        s->cur_bg     = s->saved_bg;
        s->cur_attrs  = s->saved_attrs;
        s->auto_wrap  = s->saved_wrap;
        s->pending_wrap = false;
        break;
    case 'D': /* IND – index (acts like LF) */
        do_lf(s);
        break;
    case 'E': /* NEL – next line */
        s->cur_col = 0;
        do_lf(s);
        break;
    case 'H': /* HTS – set tab stop */
        tab_set(p, s->cur_col);
        break;
    case 'M': /* RI – reverse index */
        do_ri(s);
        break;
    case 'c': /* RIS – full reset (and back to ASCII) */
        p->g0_dec = false;
        scr_free(s);
        scr_init(s, s->rows, s->cols, true);
        tabs_reset(p);
        break;
    case '\\': /* ST – string terminator (already handled inline) */
        break;
    default:
        break;
    }
}

/* ================================================================== */
/* UTF-8 byte-at-a-time decoder                                        */
/* ================================================================== */

/*
 * Feed one byte into the carry buffer.
 * Returns  1 if a complete codepoint is available in *out.
 *          0 if more bytes are needed.
 *         -1 on an invalid byte or sequence: nothing is drawn for it (the
 *            decoder is reset).
 *         -2 on a sequence cut short by a byte that is not a continuation
 *            byte: the decoder is reset, and that byte is to be fed again,
 *            as it may start the next character.
 * Overlong forms, surrogates and values past U+10FFFF are invalid.
 */
static int utf8_feed(VTParser *p, uint8_t byte, uint32_t *out)
{
    if (p->utf8_want == 0) {
        /* Start of a new codepoint */
        if (byte < 0x80) {
            *out = byte;
            return 1;
        } else if ((byte & 0xE0) == 0xC0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            p->utf8_buf[0] = byte; p->utf8_len = 1; p->utf8_want = 4;
        } else {
            return -1; /* invalid lead byte */
        }
        return 0;
    }

    /* Continuation byte expected */
    if ((byte & 0xC0) != 0x80) {
        p->utf8_want = 0;
        p->utf8_len  = 0;
        return -2;
    }

    p->utf8_buf[p->utf8_len++] = byte;
    if (p->utf8_len < p->utf8_want)
        return 0; /* still need more */

    /* Decode complete multi-byte sequence */
    uint8_t *b = p->utf8_buf;
    uint32_t cp;
    switch (p->utf8_want) {
    case 2: cp = ((uint32_t)(b[0] & 0x1F) <<  6) |  (b[1] & 0x3F); break;
    case 3: cp = ((uint32_t)(b[0] & 0x0F) << 12) | ((uint32_t)(b[1] & 0x3F) << 6)
               |  (b[2] & 0x3F); break;
    default:
    case 4: cp = ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12)
               | ((uint32_t)(b[2] & 0x3F) <<  6) |  (b[3] & 0x3F); break;
    }
    unsigned want = p->utf8_want;
    p->utf8_want = 0;
    p->utf8_len  = 0;
    if (cp < (want == 2 ? 0x80u : want == 3 ? 0x800u : 0x10000u) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return -1;
    *out = cp;
    return 1;
}

/* ================================================================== */
/* CSI parameter accumulation helpers                                  */
/* ================================================================== */

static void csi_clear(VTParser *p)
{
    memset(p->params, 0, sizeof(p->params));
    p->num_params = 0;
    memset(p->inter, 0, sizeof(p->inter));
    p->num_inter  = 0;
    p->priv       = false;
}

static void csi_digit(VTParser *p, int digit)
{
    if (p->num_params == 0) p->num_params = 1;
    if (p->params[p->num_params - 1] < 16384) /* overflow guard */
        p->params[p->num_params - 1] = p->params[p->num_params - 1] * 10 + digit;
}

static void csi_sep(VTParser *p)
{
    if (p->num_params == 0) p->num_params = 1;   /* empty first field */
    if (p->num_params < VT_MAX_PARAMS) p->num_params++;
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void vt_init(VTParser *p, int rows, int cols)
{
    memset(p, 0, sizeof(*p));
    scr_init(&p->scr, rows, cols, true);
    tabs_reset(p);
    p->state = ST_GROUND;
}

void vt_init_view(VTParser *p, int rows, int cols)
{
    memset(p, 0, sizeof(*p));
    scr_init(&p->scr, rows, cols, false);
    tabs_reset(p);
    p->state = ST_GROUND;
}

void vt_free(VTParser *p)
{
    scr_free(&p->scr);
}

/* ------------------------------------------------------------------ */
/* Screen resize                                                        */
/* ------------------------------------------------------------------ */

/*
 * Allocate a new full-size grid at (new_rows × new_cols), copy as much
 * content from g (which is full-size) as fits (top-left anchored), fill remainder with blank
 * cells, then release the old grid.  Handles NULL old_grid (first use).
 *
 * Caller must pass the OLD rows/cols so cells_dontneed can find the
 * data block inside the single allocation before we free it.
 */
static void resize_grid(Grid *g, int old_rows, int old_cols,
                        int new_rows, int new_cols)
{
    Cell **new_grid = alloc_cells(new_rows, new_cols);
    if (!new_grid) return;   /* OOM: keep old grid unchanged */

    if (g->row) {
        Cell **old_grid = (Cell **)g->row;
        int copy_rows = old_rows < new_rows ? old_rows : new_rows;
        int copy_cols = old_cols < new_cols ? old_cols : new_cols;
        for (int r = 0; r < copy_rows; r++) {
            memcpy(new_grid[r], old_grid[r], (size_t)copy_cols * sizeof(Cell));
            if (copy_cols < old_cols)           /* a wide character cut off at the edge */
                heal(new_grid[r], copy_cols - 1, copy_cols,
                     (Cell){ .ch = ' ', .flags = CELL_FG_DFL | CELL_BG_DFL });
        }

        cells_dontneed(g, old_rows, old_cols);
        free(g->row);
    }
    g->row = (void **)new_grid;
}

/*
 * Resize the Screen's cell grids and fix up all cursor/region state.
 * Called with the new terminal dimensions (not including the status row;
 * that adjustment is done by the caller).
 */
#if SCROLLBACK_ENABLED
/* ================================================================== */
/* Reflow                                                              */
/* ================================================================== */

/*
 * Resizing the primary screen re-lays its text out from logical lines: a
 * run of rows joined by ROW_WRAPPED is one line, however the rows were cut
 * (ROW_WRAPPED is set only where a wrap happened, see put_char).  The lines
 * are written top down at the new width, so each keeps its place in order:
 * a reflowed line that grows pushes the lines below it down, and one that
 * shrinks lets them move up.  Rows that no longer fit above the bottom go
 * into the scrollback, as scrolled-off rows do; a taller screen takes lines
 * back from the scrollback, as it always has.  The cursor stays on the
 * character it was on.
 *
 * The scrollback holds logical lines as text, and a line that continues on
 * the screen has no newline yet (see scrollback_capture).  That open line is
 * taken back first, as the start of row 0, so that nothing is split.
 */

static bool row_wraps(const Screen *s, int r)
{
    return s->row_flags && (s->row_flags[r] & ROW_WRAPPED);
}

static bool cell_blank(Cell c)
{
    return !(c.flags & CELL_WIDE_CONT) && (c.ch == 0 || c.ch == ' ') &&
           (c.flags & CELL_BG_DFL) &&
           !(c.attrs & (ATTR_UNDERLINE | ATTR_STRIKE | ATTR_REVERSE));
}

/* The UTF-8 text b[0..n) as cells, as put_char would have stored it (a wide
 * character as a pair).  Returns the cell count, or -1 if it would not fit
 * in cap. */
static int decode_text(const char *b, int n, Cell *out, int cap)
{
    int k = 0;
    for (int i = 0; i < n; ) {
        unsigned char b0 = (unsigned char)b[i];
        uint32_t cp; int len;
        if      (b0 < 0x80) { cp = b0;        len = 1; }
        else if (b0 < 0xE0) { cp = b0 & 0x1F; len = 2; }
        else if (b0 < 0xF0) { cp = b0 & 0x0F; len = 3; }
        else                { cp = b0 & 0x07; len = 4; }
        for (int j = 1; j < len && i + j < n; j++)
            cp = (cp << 6) | ((unsigned char)b[i + j] & 0x3F);
        i += len;
        int w = cp_width(cp);
        if (!w) continue;
        if (k + w > cap) return -1;
        out[k] = (Cell){ .ch = cp, .flags = CELL_FG_DFL | CELL_BG_DFL | (w == 2 ? CELL_WIDE : 0) };
        if (w == 2) out[k + 1] = (Cell){ .flags = CELL_FG_DFL | CELL_BG_DFL | CELL_WIDE_CONT };
        k += w;
    }
    return k;
}

/*
 * Lay one logical line (n cells at ln) out at nc columns, from output row
 * `row`: it wraps before a character that does not fit (a wide one never
 * splits), and each row that goes on into the next gets ROW_WRAPPED (and the
 * padding a wide character leaves at the margin is CELL_PAD).  With tg == NULL
 * it only counts.  Returns the rows used (at least one).
 */
static int lay_line(const Cell *ln, int n, int nc, Cell **tg, uint8_t *tf, int row)
{
    int r = 0, c = 0;
    for (int i = 0; i < n; ) {
        int w = (ln[i].flags & CELL_WIDE) ? 2 : 1;
        if (c + w > nc) {                  /* only a wide char wraps early: its margin is padding */
            if (tg) {
                tf[row + r] |= ROW_WRAPPED;
                for (int k = c; k < nc; k++) tg[row + r][k].flags |= CELL_PAD;
            }
            r++; c = 0;
        }
        if (tg) for (int k = 0; k < w; k++) tg[row + r][c + k] = ln[i + k];
        c += w; i += w;
    }
    return r + 1;
}

/*
 * Reflow the primary screen to nr x nc (nc >= 2, full scroll region).
 * Returns false, with the screen untouched, if memory runs out.
 *
 * The cursor's line is not reflowed.  The shell at its prompt (readline)
 * repaints its input after a resize, moving up by the rows it last drew the
 * input over; that count is the cursor's distance below the line's first
 * row, and it is kept: the line keeps its rows, its text cut or padded to the
 * new width, and the shell draws the input again.  Every other line reflows:
 * the lines above the cursor's line move with it, the ones below it follow.
 * Rows above the new bottom go to the scrollback, as scrolled-off rows do,
 * and a taller screen takes whole lines back from it.  A line that is cut
 * by a narrower screen is not restored until its program redraws it.
 */
static bool reflow(Screen *s, int nr, int nc)
{
    const int or_ = s->rows, oc = s->cols;
    char *buf = s->scrollback;
    const int len = s->scrollback_len;

    /* The open line at the end of the scrollback, if any, starts at cut; it
     * continues the screen's row 0, whatever is done to that row. */
    int cut = len;
    while (cut > 0 && buf[cut - 1] != '\n') cut--;

    /* The cursor's line: rows cf..cl, joined by ROW_WRAPPED. */
    int cf = s->cur_row, cl = s->cur_row;
    while (cf > 0 && row_wraps(s, cf - 1)) cf--;
    while (cl + 1 < or_ && row_wraps(s, cl)) cl++;
    const int R = cl - cf + 1;

    /* Allocate everything first, so that failure changes nothing. */
    Cell    *lin    = malloc(((size_t)or_ * oc + 2) * sizeof(Cell));
    int     *lstart = malloc((size_t)or_ * sizeof(int));
    int     *llen   = malloc((size_t)or_ * sizeof(int));
    Cell    *tmp    = malloc(((size_t)nr * nc + 2) * sizeof(Cell));
    Cell   **ng     = alloc_cells(nr, nc);
    uint8_t *nf     = calloc((size_t)nr, 1);
    if (!lin || !lstart || !llen || !tmp || !ng || !nf) goto fail;

    /* 1. The other lines of the screen, as logical lines in lin, without
     *    padding and with the blanks at each end trimmed off.  frozen is
     *    the number of lines above the cursor's. */
    int pos = 0, nl = 0, frozen = -1;
    for (int r = 0; r < or_; ) {
        if (r == cf) { frozen = nl; r = cl + 1; continue; }
        int start = pos, b = r;
        for (;;) {
            for (int c = 0; c < oc; c++) {
                const Cell *x = &((Cell **)s->g.row)[b][c];
                if (!(x->flags & CELL_PAD)) lin[pos++] = *x;
            }
            if (b + 1 >= or_ || !row_wraps(s, b) || b + 1 == cf) break;
            b++;
        }
        int n = pos - start;
        while (n > 0 && cell_blank(lin[start + n - 1])) n--;
        lstart[nl] = start; llen[nl] = n;
        nl++;
        r = b + 1;
    }
    if (frozen < 0) frozen = nl;               /* the cursor's line is always on the screen */
    /* Blank lines below the cursor's line are not text. */
    while (nl > frozen && llen[nl - 1] == 0) nl--;

    /* 2. Rows the screen's lines take at the new width. */
    int Sb = 0, Sa = 0;
    for (int i = 0; i < nl; i++) {
        int rows = lay_line(lin + lstart[i], llen[i], nc, NULL, NULL, 0);
        if (i < frozen) Sb += rows; else Sa += rows;
    }
    const int S = Sb + R + Sa;

    /* 3. A taller screen takes back whole lines from the end of the scrollback,
     *    as long as they fit beside the screen's own.  Not while there is an open
     *    line: it continues row 0, so nothing can come between it and the screen. */
    int hrows = 0, pull = cut;
    if (nr > or_ && cut == len) {
        const int avail = nr - S;
        for (int p = cut; p > 0; ) {
            int q = p - 1;                 /* buf[p-1] is the newline ending the line */
            while (q > 0 && buf[q - 1] != '\n') q--;
            int n = decode_text(buf + q, p - 1 - q, tmp, nr * nc + 2);
            if (n < 0) break;
            int rows = lay_line(tmp, n, nc, NULL, NULL, 0);
            if (hrows + rows > avail) break;
            hrows += rows;
            pull = p = q;
        }
    }

    /* 4. Lay everything out: the pulled lines, the lines above the cursor's, the
     *    cursor's own rows (cut or padded), then the lines below it. */
    const int total = hrows + S;
    Cell    **tg = alloc_cells(total, nc);
    uint8_t  *tf = calloc((size_t)total, 1);
    if (!tg || !tf) { free_cells(tg, total); free(tf); goto fail; }
    int row = 0;
    for (int p = pull; p < cut; ) {
        int e = p;
        while (e < cut && buf[e] != '\n') e++;
        int n = decode_text(buf + p, e - p, tmp, nr * nc + 2);
        row += lay_line(tmp, n, nc, tg, tf, row);
        p = e + 1;
    }
    for (int i = 0; i < nl; i++) {
        if (i == frozen) {
            for (int k = 0; k < R; k++, row++) {
                const int w = oc < nc ? oc : nc;
                for (int c = 0; c < w; c++) {
                    Cell x = ((Cell **)s->g.row)[cf + k][c];
                    if (c == nc - 1 && (x.flags & CELL_WIDE)) x = blank_cell(s);   /* cut in two */
                    tg[row][c] = x;
                }
                if (row_wraps(s, cf + k)) tf[row] |= ROW_WRAPPED;
            }
        }
        row += lay_line(lin + lstart[i], llen[i], nc, tg, tf, row);
    }
    if (frozen == nl) {                        /* the cursor's line comes last */
        for (int k = 0; k < R; k++, row++) {
            const int w = oc < nc ? oc : nc;
            for (int c = 0; c < w; c++) {
                Cell x = ((Cell **)s->g.row)[cf + k][c];
                if (c == nc - 1 && (x.flags & CELL_WIDE)) x = blank_cell(s);
                tg[row][c] = x;
            }
            if (row_wraps(s, cf + k)) tf[row] |= ROW_WRAPPED;
        }
    }

    const int cur_abs = hrows + Sb + (s->cur_row - cf);

    /* 5. Rows above the new bottom go to the scrollback, but never the cursor's row. */
    int over = total > nr ? total - nr : 0;
    if (over > cur_abs) over = cur_abs;
    const int keep = total - over < nr ? total - over : nr;

    /* Nothing below can fail: commit. */
    const bool pend = s->pending_wrap;
    const int  ccol = s->cur_col + (pend ? 1 : 0);

    s->scrollback_len = pull < cut ? pull : len;   /* history taken: back to its line; else all stays */
    s->cols = nc;
    for (int r = 0; r < over; r++)
        scrollback_capture(s, tg[r], tf[r]);
    for (int r = 0; r < keep; r++) {
        memcpy(ng[r], tg[over + r], (size_t)nc * sizeof(Cell));
        nf[r] = tf[over + r];
    }
    free_cells(tg, total);
    free(tf);
    free(lin); free(lstart); free(llen); free(tmp);

    if (s->ag.row) resize_grid(&s->ag, or_, oc, nr, nc);
    cells_dontneed(&s->g, or_, oc);
    free(s->g.row);
    s->g.row = (void **)ng;
    free(s->row_flags);
    s->row_flags = nf;
    s->rows = nr;
    s->scroll_top = 0;
    s->scroll_bottom = nr - 1;

    int crow = cur_abs - over;
    s->cur_row = crow < nr ? crow : nr - 1;
    s->cur_col = ccol < nc ? ccol : nc - 1;
    s->pending_wrap = false;
    if (s->saved_row     >= nr) s->saved_row     = nr - 1;
    if (s->saved_col     >= nc) s->saved_col     = nc - 1;
    if (s->alt_saved_row >= nr) s->alt_saved_row = nr - 1;
    if (s->alt_saved_col >= nc) s->alt_saved_col = nc - 1;
    return true;

fail:
    free(lin); free(lstart); free(llen); free(tmp);
    free_cells(ng, nr); free(nf);
    return false;
}
#endif /* SCROLLBACK_ENABLED */

static void scr_resize_wide(Screen *s, int new_rows, int new_cols)
{
    int old_rows = s->rows;
    int old_cols = s->cols;

#if SCROLLBACK_ENABLED
    if (!s->in_alt_screen && s->scrollback && s->g.row && new_cols >= 2 &&
        s->scroll_top == 0 && s->scroll_bottom == old_rows - 1 &&
        reflow(s, new_rows, new_cols))
        return;
#endif

    /* Default resize (shrink with no scrollback, or alt screen) */
    resize_grid(&s->g, old_rows, old_cols, new_rows, new_cols);
    if (s->ag.row)
        resize_grid(&s->ag, old_rows, old_cols, new_rows, new_cols);

    /* Resize row_flags: realloc and zero any new rows */
    if (s->row_flags) {
        s->row_flags = realloc(s->row_flags, (size_t)new_rows);
        if (s->row_flags && new_rows > old_rows)
            memset(s->row_flags + old_rows, 0,
                   (size_t)(new_rows - old_rows));
    }

    s->rows = new_rows;
    s->cols = new_cols;

    /* ---- Clamp cursor ---- */
    if (s->cur_row >= new_rows) { s->cur_row = new_rows - 1; s->pending_wrap = false; }
    if (s->cur_col >= new_cols) { s->cur_col = new_cols - 1; s->pending_wrap = false; }

    /* ---- Scroll region ----
     * If the region spanned the full old screen, extend it to the full
     * new screen.  Otherwise just clamp the bottom to the new last row. */
    if (s->scroll_top == 0 && s->scroll_bottom == old_rows - 1) {
        s->scroll_bottom = new_rows - 1;
    } else {
        if (s->scroll_top  >= new_rows) s->scroll_top  = 0;
        if (s->scroll_bottom >= new_rows) s->scroll_bottom = new_rows - 1;
        if (s->scroll_top > s->scroll_bottom)
            s->scroll_bottom = new_rows - 1;   /* degenerate: reset */
    }

    /* ---- Clamp saved cursors ---- */
    if (s->saved_row     >= new_rows) s->saved_row     = new_rows - 1;
    if (s->saved_col     >= new_cols) s->saved_col     = new_cols - 1;
    if (s->alt_saved_row >= new_rows) s->alt_saved_row = new_rows - 1;
    if (s->alt_saved_col >= new_cols) s->alt_saved_col = new_cols - 1;
}

/* Resize both grids, which the resizing above does only to full-size ones: so
 * they are made that, and small again once resized, if what they hold fits.
 * (Out of memory for that: the screen stays as it was.) */
static void scr_resize(Screen *s, int new_rows, int new_cols)
{
    s->hold = s->flips = 0;
    if (!grid_widen(s, &s->g) || (s->ag.row && !grid_widen(s, &s->ag))) return;
    scr_resize_wide(s, new_rows, new_cols);
    grid_compact(s, &s->g);
    if (s->ag.row) grid_compact(s, &s->ag);
}


void vt_resize(VTParser *p, int rows, int cols)
{
    if (rows == p->scr.rows && cols == p->scr.cols) return;

    int old_rows = p->scr.rows;
    scr_resize(&p->scr, rows, cols);
    if (p->scr.alt_row_flags) {
        uint8_t *f = realloc(p->scr.alt_row_flags, (size_t)rows);
        if (f) {
            if (rows > old_rows) memset(f + old_rows, 0, (size_t)(rows - old_rows));
            p->scr.alt_row_flags = f;
        }
    }

    /* Rebuild tab stops for the new column count.  scr_resize has already
     * updated p->scr.cols, so tabs_reset will use the new width. */
    tabs_reset(p);
}
/* ================================================================== */
/* Serialization                                                        */
/* ================================================================== */

#include <unistd.h>
#include <errno.h>


static int vt_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int vt_read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = ECONNRESET; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

/*
 * Packed wire structs — no padding, explicit widths.
 * Used only for serialize/deserialize; never stored on disk.
 */
typedef struct __attribute__((packed)) {
    int32_t  state;
    int32_t  params[VT_MAX_PARAMS];
    int32_t  num_params;
    char     inter[VT_MAX_INTER];
    int32_t  num_inter;
    uint8_t  priv;
    int32_t  osc_len;
    uint8_t  utf8_buf[4];
    int32_t  utf8_len;
    int32_t  utf8_want;
    uint8_t  tabstops[VT_MAX_COLS / 8 + 1];
    uint8_t  cpr_requested;
    uint8_t  g0_dec, esc_inter;   /* the character set, and an escape in progress */
} VTWire;

typedef struct __attribute__((packed)) {
    int32_t  rows, cols;
    int32_t  cur_row, cur_col;
    uint8_t  cur_visible, pending_wrap;
    uint8_t  origin_mode, auto_wrap, app_cursor, bracketed_paste;
    uint8_t  insert_mode, in_alt_screen;
    uint8_t  mouse_mode, mouse_enc;
    int32_t  scroll_top, scroll_bottom;
    uint16_t cur_fg, cur_bg;
    uint8_t  cur_attrs;
    int32_t  saved_row, saved_col;
    uint16_t saved_fg, saved_bg;
    uint8_t  saved_attrs, saved_wrap;
    int32_t  alt_saved_row, alt_saved_col;
    uint16_t alt_saved_fg, alt_saved_bg;
    uint8_t  alt_saved_attrs;
    char     title[256];
} ScreenWire;

int vt_serialize(const VTParser *p, int fd)
{
    /* VT parser metadata */
    VTWire vw;
    memset(&vw, 0, sizeof(vw));
    vw.state      = (int32_t)p->state;
    vw.num_params = (int32_t)p->num_params;
    vw.num_inter  = (int32_t)p->num_inter;
    vw.priv       = p->priv ? 1 : 0;
    vw.osc_len    = (int32_t)p->osc_len;
    vw.utf8_len   = (int32_t)p->utf8_len;
    vw.utf8_want  = (int32_t)p->utf8_want;
    vw.cpr_requested = p->cpr_requested ? 1 : 0;
    vw.g0_dec     = p->g0_dec ? 1 : 0;
    vw.esc_inter  = p->esc_inter;
    for (int i = 0; i < VT_MAX_PARAMS; i++) vw.params[i] = (int32_t)p->params[i];
    memcpy(vw.inter,    p->inter,    VT_MAX_INTER);
    memcpy(vw.utf8_buf, p->utf8_buf, 4);
    memcpy(vw.tabstops, p->tabstops, sizeof(p->tabstops));
    if (vt_write_all(fd, &vw, sizeof(vw)) < 0) return -1;

    /* OSC buffer (full fixed-size block; osc_len tells how much is valid) */
    if (vt_write_all(fd, p->osc, VT_MAX_OSC) < 0) return -1;

    /* Screen metadata */
    const Screen *s = &p->scr;
    ScreenWire sw;
    memset(&sw, 0, sizeof(sw));
    sw.rows           = (int32_t)s->rows;
    sw.cols           = (int32_t)s->cols;
    sw.cur_row        = (int32_t)s->cur_row;
    sw.cur_col        = (int32_t)s->cur_col;
    sw.cur_visible    = s->cur_visible    ? 1 : 0;
    sw.pending_wrap   = s->pending_wrap   ? 1 : 0;
    sw.origin_mode    = s->origin_mode    ? 1 : 0;
    sw.auto_wrap      = s->auto_wrap      ? 1 : 0;
    sw.app_cursor     = s->app_cursor     ? 1 : 0;
    sw.bracketed_paste = s->bracketed_paste ? 1 : 0;
    sw.mouse_mode     = s->mouse_mode;
    sw.mouse_enc      = s->mouse_enc;
    sw.insert_mode    = s->insert_mode    ? 1 : 0;
    sw.in_alt_screen  = s->in_alt_screen  ? 1 : 0;
    sw.scroll_top     = (int32_t)s->scroll_top;
    sw.scroll_bottom  = (int32_t)s->scroll_bottom;
    sw.cur_fg         = s->cur_fg;
    sw.cur_bg         = s->cur_bg;
    sw.cur_attrs      = s->cur_attrs;
    sw.saved_row      = (int32_t)s->saved_row;
    sw.saved_col      = (int32_t)s->saved_col;
    sw.saved_fg       = s->saved_fg;
    sw.saved_bg       = s->saved_bg;
    sw.saved_attrs    = s->saved_attrs;
    sw.saved_wrap     = s->saved_wrap     ? 1 : 0;
    sw.alt_saved_row  = (int32_t)s->alt_saved_row;
    sw.alt_saved_col  = (int32_t)s->alt_saved_col;
    sw.alt_saved_fg   = s->alt_saved_fg;
    sw.alt_saved_bg   = s->alt_saved_bg;
    sw.alt_saved_attrs = s->alt_saved_attrs;
    memcpy(sw.title, s->title, 256);
    if (vt_write_all(fd, &sw, sizeof(sw)) < 0) return -1;

    /*
     * Cell data: write rows in LOGICAL order (rows 0..rows-1 of s->g) not
     * physical memory order.  Scrolling shuffles the row pointer array
     * without moving cell data, so physical order may be rotated relative
     * to logical order.  Deserializing physical order back into a fresh
     * sequential grid would produce a circularly-shifted screen.
     */
    size_t row_bytes = (size_t)s->cols * sizeof(Cell);
    for (int r = 0; r < s->rows; r++)
        if (vt_write_all(fd, grow(s, &s->g, r, 0), row_bytes) < 0) return -1;

    /* Alt cell data (also logical order) */
    uint8_t has_alt = s->ag.row ? 1 : 0;
    if (vt_write_all(fd, &has_alt, 1) < 0) return -1;
    if (has_alt)
        for (int r = 0; r < s->rows; r++)
            if (vt_write_all(fd, grow(s, &s->ag, r, 0), row_bytes) < 0) return -1;

    return 0;
}

/* The rows of grid g, read from fd as Cell[cols] each. */
static int read_grid(Screen *s, Grid *g, int fd)
{
    size_t row_bytes = (size_t)s->cols * sizeof(Cell);
    Cell *buf = malloc(row_bytes);
    if (!buf) return -1;
    for (int r = 0; r < s->rows; r++) {
        if (vt_read_all(fd, buf, row_bytes) < 0) { free(buf); return -1; }
        for (int c = 0; c < s->cols; c++) heal(buf, c, s->cols, blank_cell(s));
        row_store(s, g, r, buf);
    }
    free(buf);
    return 0;
}

int vt_deserialize(VTParser *p, int fd)
{
    VTWire vw;
    if (vt_read_all(fd, &vw, sizeof(vw)) < 0) return -1;

    char osc_buf[VT_MAX_OSC];
    if (vt_read_all(fd, osc_buf, VT_MAX_OSC) < 0) return -1;

    ScreenWire sw;
    if (vt_read_all(fd, &sw, sizeof(sw)) < 0) return -1;

    /* Re-initialize at the stored dimensions */
    vt_free(p);
    vt_init(p, (int)sw.rows, (int)sw.cols);

    /* Restore VT parser state */
    p->state         = (VTState)vw.state;
    p->num_params    = (int)vw.num_params;
    p->num_inter     = (int)vw.num_inter;
    p->priv          = vw.priv;
    p->osc_len       = (int)vw.osc_len;
    p->utf8_len      = (int)vw.utf8_len;
    p->utf8_want     = (int)vw.utf8_want;
    p->cpr_requested = vw.cpr_requested;
    p->g0_dec        = vw.g0_dec != 0;
    p->esc_inter     = vw.esc_inter;
    for (int i = 0; i < VT_MAX_PARAMS; i++) p->params[i] = (int)vw.params[i];
    memcpy(p->inter,    vw.inter,    VT_MAX_INTER);
    memcpy(p->utf8_buf, vw.utf8_buf, 4);
    memcpy(p->tabstops, vw.tabstops, sizeof(p->tabstops));
    memcpy(p->osc,      osc_buf,     VT_MAX_OSC);

    /* Restore screen metadata */
    Screen *s        = &p->scr;
    s->cur_row       = (int)sw.cur_row;
    s->cur_col       = (int)sw.cur_col;
    s->cur_visible   = sw.cur_visible;
    s->pending_wrap  = sw.pending_wrap;
    s->origin_mode   = sw.origin_mode;
    s->auto_wrap     = sw.auto_wrap;
    s->app_cursor    = sw.app_cursor;
    s->bracketed_paste = sw.bracketed_paste;
    s->mouse_mode    = sw.mouse_mode;
    s->mouse_enc     = sw.mouse_enc;
    s->insert_mode   = sw.insert_mode;
    s->in_alt_screen = sw.in_alt_screen;
    s->scroll_top    = (int)sw.scroll_top;
    s->scroll_bottom = (int)sw.scroll_bottom;
    s->cur_fg        = sw.cur_fg;
    s->cur_bg        = sw.cur_bg;
    s->cur_attrs     = sw.cur_attrs;
    s->saved_row     = (int)sw.saved_row;
    s->saved_col     = (int)sw.saved_col;
    s->saved_fg      = sw.saved_fg;
    s->saved_bg      = sw.saved_bg;
    s->saved_attrs   = sw.saved_attrs;
    s->saved_wrap    = sw.saved_wrap;
    s->alt_saved_row = (int)sw.alt_saved_row;
    s->alt_saved_col = (int)sw.alt_saved_col;
    s->alt_saved_fg  = sw.alt_saved_fg;
    s->alt_saved_bg  = sw.alt_saved_bg;
    s->alt_saved_attrs = sw.alt_saved_attrs;
    memcpy(s->title, sw.title, 256);

    /*
     * Read cell data row by row in logical order.  The fresh grid from
     * vt_init has sequential row pointers, which is exactly the logical
     * order we serialized, so each row lands in the right place.
     */
    if (read_grid(s, &s->g, fd) < 0) return -1;

    /* Alt cells */
    uint8_t has_alt;
    if (vt_read_all(fd, &has_alt, 1) < 0) return -1;
    if (has_alt) {
        if (!s->ag.row && !grid_new(&s->ag, s->rows, s->cols)) return -1;
        if (read_grid(s, &s->ag, fd) < 0) return -1;
    }

    return 0;
}


void vt_feed(VTParser *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        /*
         * CAN (0x18) and SUB (0x1A) cancel the current sequence anywhere
         * except GROUND (where they'd just be discarded anyway).
         */
        if ((c == 0x18 || c == 0x1A) && p->state != ST_GROUND) {
            p->state = ST_GROUND;
            continue;
        }

        /*
         * ESC (0x1B) interrupts and restarts a new sequence from any state.
         * Exception: if we're mid-OSC, ESC might begin the ST (ESC \).
         * We handle that by transitioning to ST_ESC; the following '\' will
         * be a harmless no-op in dispatch_esc.
         */
        if (c == 0x1B) {
            if (p->state == ST_OSC) dispatch_osc(p);
            csi_clear(p);
            p->state = ST_ESC;
            continue;
        }

        switch (p->state) {

        /* ---- GROUND ---- */
        case ST_GROUND:
            if (c < 0x20 || c == 0x7F) {
                p->utf8_want = 0;       /* a control ends a sequence cut short */
                if (c != 0x7F) handle_c0(p, c);     /* DEL is ignored */
            } else {
                uint32_t cp;
                int r = utf8_feed(p, c, &cp);
                if (r == -2) r = utf8_feed(p, c, &cp);  /* c starts afresh */
                if (r == 1) {
                    if (p->g0_dec && cp >= 0x60 && cp <= 0x7e) cp = dec_graphics[cp - 0x60];
                    put_char(p, cp);
                }
                /* r == 0: partial multibyte, state held in p->utf8_* */
                /* r == -1: invalid byte or sequence, skip */
            }
            break;

        /* ---- ESC ---- */
        case ST_ESC:
            if (c == '[') {
                /* Start of CSI */
                csi_clear(p);
                p->state = ST_CSI_PARAM;
            } else if (c == ']') {
                p->osc_len = 0;
                p->state   = ST_OSC;
            } else if (c == 'P' || c == '^' || c == '_') {
                /* DCS / PM / APC – ignore until ST */
                p->state = ST_DCS;
            } else if (c >= 0x20 && c <= 0x2F) {
                /* ESC intermediate byte (e.g. charset: ESC ( B) */
                p->esc_inter = c;
                p->state = ST_ESC_INTER;
            } else if (c >= 0x30 && c <= 0x7E) {
                dispatch_esc(p, (char)c);
                p->state = ST_GROUND;
            } else {
                p->state = ST_GROUND;
            }
            break;

        /* ---- ESC + intermediate byte(s) ---- */
        case ST_ESC_INTER:
            /* Consume exactly one final byte and return to ground */
            if (c >= 0x30 && c <= 0x7E) {
                if (p->esc_inter == '(' && (c == '0' || c == 'B'))
                    p->g0_dec = (c == '0');
                p->state = ST_GROUND;
            }
            /* else stay: accumulate more intermediates (rare) */
            break;

        /* ---- CSI parameter collection ---- */
        case ST_CSI_PARAM:
            if (c >= '0' && c <= '9') {
                csi_digit(p, c - '0');
            } else if (c == ';') {
                csi_sep(p);
            } else if (c == '?') {
                p->priv = true;
            } else if (c == '>' || c == '<' || c == '=') {
                /* DEC modifier – record or ignore */
            } else if (c >= 0x20 && c <= 0x2F) {
                if (p->num_inter < VT_MAX_INTER)
                    p->inter[p->num_inter++] = (char)c;
                p->state = ST_CSI_INTER;
            } else if (c >= 0x40 && c <= 0x7E) {
                dispatch_csi(p, (char)c);
                p->state = ST_GROUND;
            } else {
                /* Anything else is an error – abort */
                p->state = ST_GROUND;
            }
            break;

        /* ---- CSI intermediate bytes ---- */
        case ST_CSI_INTER:
            if (c >= 0x20 && c <= 0x2F) {
                if (p->num_inter < VT_MAX_INTER)
                    p->inter[p->num_inter++] = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                dispatch_csi(p, (char)c);
                p->state = ST_GROUND;
            } else {
                p->state = ST_GROUND;
            }
            break;

        /* ---- OSC string ---- */
        case ST_OSC:
            if (c == 0x07) {   /* BEL terminates OSC (xterm extension) */
                dispatch_osc(p);
                p->state = ST_GROUND;
            } else if (c == 0x9C) { /* 8-bit ST */
                dispatch_osc(p);
                p->state = ST_GROUND;
            } else {
                if (p->osc_len < VT_MAX_OSC - 1)
                    p->osc[p->osc_len++] = (char)c;
            }
            break;

        /* ---- DCS / PM / APC – wait for ST ---- */
        case ST_DCS:
            if (c == 0x07 || c == 0x9C)
                p->state = ST_GROUND;
            /* ESC handled at top → ST_ESC; '\' will be a no-op */
            break;
        }
    }
}
