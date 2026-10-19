#include "render.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#ifdef X11_BACKEND
#include "x11_backend.h"
#endif

/* ================================================================== */
/* Output buffer                                                        */
/* ================================================================== */

/* A frame is built here and written in one go.  16 KiB holds a full redraw
 * of a large coloured terminal in a few writes; a bigger buffer would only
 * sit in .bss (it was 128 KiB). */
#define OUTBUF_CAP (16 * 1024)
static char   outbuf[OUTBUF_CAP];
static int    outbuf_len;

static void out_flush(void)
{
    if (outbuf_len > 0) {
        (void)write(STDOUT_FILENO, outbuf, (size_t)outbuf_len);
        outbuf_len = 0;
    }
}

static void out_raw(const char *s, int n)
{
    if (outbuf_len + n > OUTBUF_CAP) out_flush();
    memcpy(outbuf + outbuf_len, s, (size_t)n);
    outbuf_len += n;
}

static void out_str(const char *s)
{
    out_raw(s, (int)strlen(s));
}

static void out_char(char c)
{
    if (outbuf_len >= OUTBUF_CAP) out_flush();
    outbuf[outbuf_len++] = c;
}

/* ================================================================== */
/* Emit helpers                                                         */
/* ================================================================== */

/* Append "ESC [ n final" (n omitted when it is 1) to t; returns its length. */
static int csi_n(char *t, int n, char final)
{
    int k = 0;
    t[k++] = '\033'; t[k++] = '[';
    if (n != 1) k += snprintf(t + k, 12, "%d", n);
    t[k++] = final;
    return k;
}

/*
 * Move the real cursor to (row, col) by the shortest sequence of the ones
 * that are certain to work: CUP, or a vertical move (LF, CUU, CUD) and then
 * a horizontal one (CR, BS, CUF, CHA).  Only moves to rows that exist on the
 * terminal are made, so an LF never scrolls.  The column counts as known
 * only when the cursor is not at the end of a row: a terminal that is
 * waiting to wrap there treats BS and CUB differently from one that is not.
 */
static void emit_move(RenderState *rs, int row, int col)
{
    int cr = rs->cursor_row, cc = rs->cursor_col;
    if (cr == row && cc == col) return;

    char best[32], t[32];
    int  bn = 0, tn = 0;
    /* CUP; a parameter of 1 may be left out */
    best[bn++] = '\033'; best[bn++] = '[';
    if (row > 0) bn += snprintf(best + bn, 12, "%d", row + 1);
    if (col > 0) { best[bn++] = ';'; bn += snprintf(best + bn, 12, "%d", col + 1); }
    best[bn++] = 'H';

    if (cr >= 0) {
        int kc = (cc >= 0 && cc < rs->cols) ? cc : -1;     /* known column */
        int dr = row - cr;
        if (dr > 0 && dr <= 3) { memset(t, '\n', (size_t)dr); tn = dr; }
        else if (dr != 0)      tn = csi_n(t, dr > 0 ? dr : -dr, dr > 0 ? 'B' : 'A');

        if (col == 0)                          t[tn++] = '\r';
        else if (kc >= 0 && col < kc && kc - col <= 3)
            for (int k = kc - col; k > 0; k--) t[tn++] = '\b';
        else if (kc >= 0 && col > kc)          tn += csi_n(t + tn, col - kc, 'C');
        else if (kc != col)                    tn += csi_n(t + tn, col + 1, 'G');

        if (tn < bn) { memcpy(best, t, (size_t)tn); bn = tn; }
    }
    out_raw(best, bn);
    rs->cursor_row = row;
    rs->cursor_col = col;
}

/*
 * SGR parameters for a state, each preceded by ';'.  With inc, only what
 * differs from the terminal's present state: attributes newly set, and
 * colours that changed (back to the default being 39 / 49).  Without, all
 * of it, for use after a reset (where default colours need no parameter).
 */
static int sgr_params(char *p, const RenderState *rs, bool inc,
                      uint16_t fg, uint16_t bg, uint8_t attrs)
{
    static const struct { uint8_t bit; char code; } A[] = {
        { ATTR_BOLD, '1' }, { ATTR_DIM, '2' }, { ATTR_ITALIC, '3' },
        { ATTR_UNDERLINE, '4' }, { ATTR_BLINK, '5' }, { ATTR_REVERSE, '7' },
        { ATTR_INVIS, '8' }, { ATTR_STRIKE, '9' },
    };
    int n = 0;
    uint8_t add = inc ? (uint8_t)(attrs & ~rs->cur_attrs) : attrs;
    for (unsigned i = 0; i < sizeof A / sizeof *A; i++)
        if (add & A[i].bit) { p[n++] = ';'; p[n++] = A[i].code; }

    for (int k = 0; k < 2; k++) {
        uint16_t c    = k ? bg : fg;
        int      base = k ? 40 : 30;
        if (inc && c == (k ? rs->cur_bg : rs->cur_fg)) continue;
        if (c == COLOR_DEFAULT) {
            if (inc) n += snprintf(p + n, 8, ";%d", base + 9);
        } else if (c < 8)  n += snprintf(p + n, 8, ";%d", base + c);
        else if (c < 16)   n += snprintf(p + n, 8, ";%d", base + 60 + c - 8);
        else               n += snprintf(p + n, 16, ";%d;5;%d", base + 8, c);
    }
    return n;
}

/*
 * Bring the terminal's graphic rendition to (fg, bg, attrs): the shorter of
 * a reset followed by the whole state, and, when no attribute has to be
 * turned off (there is no SGR that does that without a reset or a code per
 * attribute), just the changes.  A colour change on a coloured run is then
 * 5-10 bytes rather than 20.
 */
static void emit_sgr(RenderState *rs, uint16_t fg, uint16_t bg, uint8_t attrs)
{
    if (fg == rs->cur_fg && bg == rs->cur_bg && attrs == rs->cur_attrs) return;

    char full[64], inc[64], p[56];
    int  pn, fn = 0, in = 0;

    pn = sgr_params(p, rs, false, fg, bg, attrs);
    full[fn++] = '\033'; full[fn++] = '[';
    if (pn) full[fn++] = '0';                       /* a bare ESC [ m resets */
    memcpy(full + fn, p, (size_t)pn); fn += pn;
    full[fn++] = 'm';

    if (!(rs->cur_attrs & ~attrs) && (pn = sgr_params(p, rs, true, fg, bg, attrs)) > 0) {
        inc[in++] = '\033'; inc[in++] = '[';
        memcpy(inc + in, p + 1, (size_t)pn - 1); in += pn - 1;   /* drop the first ';' */
        inc[in++] = 'm';
    }

    if (in && in < fn) out_raw(inc, in);
    else               out_raw(full, fn);

    rs->cur_fg    = fg;
    rs->cur_bg    = bg;
    rs->cur_attrs = attrs;
}

/* Encode a Unicode codepoint as UTF-8 and write to output buffer */
static void emit_utf8(uint32_t cp)
{
    char buf[4];
    int  n;
    if (cp < 0x80) {
        buf[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 |  (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >>  6) & 0x3F));
        buf[2] = (char)(0x80 |  (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 |  (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >>  6) & 0x3F));
        buf[3] = (char)(0x80 |  (cp & 0x3F));
        n = 4;
    }
    out_raw(buf, n);
}

/* ================================================================== */
/* Shadow buffer                                                        */
/* ================================================================== */

static Cell **alloc_shadow(int rows, int cols)
{
    Cell **s = calloc((size_t)rows, sizeof(Cell *));
    for (int r = 0; r < rows; r++) {
        s[r] = calloc((size_t)cols, sizeof(Cell));
        /* Initialise to an impossible value so the first render writes
         * every cell. */
        for (int c = 0; c < cols; c++) {
            s[r][c].ch = (uint32_t)-1;
        }
    }
    return s;
}

static void free_shadow(Cell **s, int rows)
{
    for (int r = 0; r < rows; r++) free(s[r]);
    free(s);
}

/*
 * Whether two cells look the same on the terminal.  A never-written cell
 * has ch 0, which is a blank, the same as ' '.  A colour that is the
 * default is the default whatever index it is stored with.
 */
static bool cells_eq(const Cell *a, const Cell *b)
{
    uint32_t ca = a->ch ? a->ch : ' ';
    uint32_t cb = b->ch ? b->ch : ' ';
    if (ca != cb || a->attrs != b->attrs || a->flags != b->flags)
        return false;
    if (!(a->flags & CELL_FG_DFL) && a->fg != b->fg) return false;
    if (!(a->flags & CELL_BG_DFL) && a->bg != b->bg) return false;
    return true;
}

/* What the terminal shows after an erase in the default colours. */
static const Cell BLANK_CELL = { .ch = ' ', .flags = CELL_FG_DFL | CELL_BG_DFL };

/* A cell that shows as nothing: a space on the default background. */
static bool cell_is_blank(const Cell *c)
{
    return !(c->flags & (CELL_WIDE | CELL_WIDE_CONT))
        && (c->ch == 0 || c->ch == ' ')
        && (c->flags & CELL_BG_DFL)
        && !(c->attrs & (ATTR_UNDERLINE | ATTR_STRIKE | ATTR_REVERSE));
}

/* Gaps this short are filled with blanks rather than moved over: a blank is
 * one byte, and a cursor move is about eight. */
#define GAP_FILL_MAX 4

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void render_init(RenderState *rs, int rows, int cols)
{
    rs->rows           = rows;
    rs->cols           = cols;
    rs->shadow             = alloc_shadow(rows + 1, cols);  /* + the status row */
    rs->status             = calloc((size_t)cols, sizeof(Cell));
    rs->status_row         = -1;    /* no status row until render_status_row() */
    rs->shadow_row_wrapped = calloc((size_t)rows, sizeof(bool));
    rs->row_hash           = calloc((size_t)rows * 2, sizeof(uint32_t));
    rs->cursor_row         = -1;   /* unknown until first emit_move */
    rs->cursor_col         = -1;
    rs->cur_fg         = COLOR_DEFAULT;
    rs->cur_bg         = COLOR_DEFAULT;
    rs->cur_attrs      = 0;
    rs->cursor_visible = true;
}

void render_free(RenderState *rs)
{
    if (!rs->shadow) return;   /* already freed or never initialised */
    free_shadow(rs->shadow, rs->rows + 1);
    rs->shadow = NULL;
    free(rs->status);
    rs->status = NULL;
    free(rs->shadow_row_wrapped);
    rs->shadow_row_wrapped = NULL;
    free(rs->row_hash);
    rs->row_hash = NULL;
    rs->rows   = 0;
    rs->cols   = 0;
}

/*
 * Scrolling.  Output that is appended at the foot of the screen moves
 * everything above it up a line or more; the diff would resend all of it.
 * Instead the terminal is told to scroll its child area (a scroll region
 * over the screen's rows, so the status row below is not touched), and the
 * shadow is moved to match.  Then only what is new is sent.
 */

/* A scroll must save at least this many changed cells to be worth its
 * fixed cost (region set, cursor move, line feeds, region reset). */
#define SCROLL_MIN_SAVING 32

/* A frame that changes at least this many cells hides the cursor while it
 * is drawn (see render_screen). */
#define HIDE_MIN_CELLS 48

static uint32_t cell_hash(const Cell *c)
{
    uint32_t ch = c->ch ? c->ch : ' ';
    uint32_t fg = (c->flags & CELL_FG_DFL) ? 0xffffu : c->fg;
    uint32_t bg = (c->flags & CELL_BG_DFL) ? 0xffffu : c->bg;
    uint32_t h = ch;
    h = h * 31u + fg;
    h = h * 31u + bg;
    h = h * 31u + c->attrs;
    h = h * 31u + c->flags;
    return h;
}

static uint32_t row_hash(const Cell *row, int cols)
{
    uint32_t h = 2166136261u;
    for (int c = 0; c < cols; c++)
        h = (h ^ cell_hash(&row[c])) * 16777619u;
    return h;
}

/* The shadow row that row r of the screen is drawn over, when the shadow
 * is moved by shift rows (up for positive): -1 where the row scrolls in
 * from outside, and so is blank. */
static int src_row(int r, int shift, int rows)
{
    int src = r + shift;
    return (src >= 0 && src < rows) ? src : -1;
}

/* How many cells the diff would send if s were drawn over the shadow moved
 * by shift rows. */
static int count_changed(const RenderState *rs, const Screen *s,
                         int rows, int cols, int shift)
{
    int n = 0;
    for (int r = 0; r < rows; r++) {
        int src = src_row(r, shift, rows);
        for (int c = 0; c < cols; c++) {
            const Cell *cell = &s->cells[r][c];
            if (cell->flags & CELL_WIDE_CONT) continue;
            const Cell *old = (src < 0) ? &BLANK_CELL : &rs->shadow[src][c];
            if (!cells_eq(cell, old)) n++;
        }
    }
    return n;
}

/* Rotate a[0..n) left by k (the first k elements move to the end). */
static void rotate_cells(Cell **a, int n, int k)
{
    for (int i = 0, j = k - 1; i < j; i++, j--) { Cell *t = a[i]; a[i] = a[j]; a[j] = t; }
    for (int i = k, j = n - 1; i < j; i++, j--) { Cell *t = a[i]; a[i] = a[j]; a[j] = t; }
    for (int i = 0, j = n - 1; i < j; i++, j--) { Cell *t = a[i]; a[i] = a[j]; a[j] = t; }
}

static void rotate_flags(bool *a, int n, int k)
{
    for (int i = 0, j = k - 1; i < j; i++, j--) { bool t = a[i]; a[i] = a[j]; a[j] = t; }
    for (int i = k, j = n - 1; i < j; i++, j--) { bool t = a[i]; a[i] = a[j]; a[j] = t; }
    for (int i = 0, j = n - 1; i < j; i++, j--) { bool t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* Move the first rows rows of the terminal by shift lines (positive: up).
 * The region is the screen's rows, so the status row below is not touched.
 * Deleting lines at its top pulls the rows below up and blanks the foot;
 * inserting lines pushes them down and blanks the head.  The new lines come
 * out in the default background.  The shadow is moved to match. */
static void scroll_lines(RenderState *rs, int rows, int shift)
{
    emit_sgr(rs, COLOR_DEFAULT, COLOR_DEFAULT, 0);
    int m = shift < 0 ? -shift : shift;
    char buf[48];
    /* Setting the region homes the cursor, which is where DL/IL must act:
     * no move of our own is needed.  Resetting it homes the cursor again. */
    int n = snprintf(buf, sizeof(buf), "\033[1;%dr\033[%d%c\033[r",
                     rows, m, shift > 0 ? 'M' : 'L');
    out_raw(buf, n);
    rs->cursor_row = 0;              /* home: known */
    rs->cursor_col = 0;

    /* Rotating left by this brings each row to where the terminal put it. */
    int left = shift > 0 ? shift : rows - m;
    rotate_cells(rs->shadow, rows, left);
    if (rs->shadow_row_wrapped)
        rotate_flags(rs->shadow_row_wrapped, rows, left);
    int first = shift > 0 ? rows - m : 0;
    for (int r = first; r < first + m; r++) {
        for (int c = 0; c < rs->cols; c++)
            rs->shadow[r][c] = BLANK_CELL;
        if (rs->shadow_row_wrapped)
            rs->shadow_row_wrapped[r] = false;
    }
}

/* If the change is mostly the screen moving (up or down), scroll the
 * terminal rather than resending it.  base is the number of cells that
 * differ now; returns how many do after. */
static int maybe_scroll(RenderState *rs, const Screen *s, int rows, int cols,
                        int base)
{
    if (rows < 2 || !rs->row_hash || base < SCROLL_MIN_SAVING) return base;

    uint32_t *hnew = rs->row_hash, *hold = rs->row_hash + rs->rows;
    for (int r = 0; r < rows; r++) {
        hnew[r] = row_hash(s->cells[r], cols);
        hold[r] = row_hash(rs->shadow[r], cols);
    }

    /* The shift (either direction) that matches the most rows of the new
     * screen against the shadow; a tie goes to the smaller shift, upward
     * first.  Hash equality is only a guide: the cell count below decides,
     * so a collision cannot make it wrong. */
    int best = 0, best_m = 0;
    for (int d = 1; d < rows; d++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            int shift = sign * d, m = 0;
            for (int r = 0; r < rows; r++) {
                int src = src_row(r, shift, rows);
                if (src >= 0 && hold[src] == hnew[r]) m++;
            }
            if (m > best_m) { best_m = m; best = shift; }
        }
    }
    if (best == 0 || best_m < 2) return base;

    int after = count_changed(rs, s, rows, cols, best);
    if (base - after < SCROLL_MIN_SAVING) return base;
    scroll_lines(rs, rows, best);
    return after;
}

/* ================================================================== */
/* The status row                                                       */
/* ================================================================== */

/* How many cells of the status row differ from what the terminal shows. */
static int status_changed(const RenderState *rs)
{
    if (!rs->status || rs->status_row < 0) return 0;
    int n = 0;
    const Cell *shadow = rs->shadow[rs->rows];
    for (int c = 0; c < rs->cols; c++)
        if (!cells_eq(&rs->status[c], &shadow[c])) n++;
    return n;
}

Cell *render_status_row(RenderState *rs, int row)
{
    if (!rs->status) return NULL;
    if (row != rs->status_row) {
        /* A new place: whatever was shown at the old one is not ours to
         * rely on, and nothing is known about the new one. */
        for (int c = 0; c < rs->cols; c++) rs->shadow[rs->rows][c].ch = (uint32_t)-1;
        rs->status_row = row;
    }
    for (int c = 0; c < rs->cols; c++) rs->status[c] = BLANK_CELL;
    return rs->status;
}

/*
 * Print one cell.  With move false the cell is printed where the terminal
 * already is (the first cell of a row, after a wrap).  The shadow records
 * the cell, and the cursor belief moves past it.  The caller deals with a
 * cell that reaches the last column.
 */
static void put_cell(RenderState *rs, int r, int c, const Cell *cell,
                     Cell *shadow, int cols, bool move)
{
    if (move) emit_move(rs, r, c);
    uint16_t efg = (cell->flags & CELL_FG_DFL) ? COLOR_DEFAULT : cell->fg;
    uint16_t ebg = (cell->flags & CELL_BG_DFL) ? COLOR_DEFAULT : cell->bg;
    emit_sgr(rs, efg, ebg, cell->attrs);

    if (cell->ch == 0 || cell->ch == ' ') {
        out_char(' ');
        if (cell->flags & CELL_WIDE) out_char(' '); /* fill continuation col */
    } else {
        emit_utf8(cell->ch);
    }

    *shadow = *cell;
    if ((cell->flags & CELL_WIDE) && c + 1 < cols) {
        Cell cont  = *cell;
        cont.flags = (cont.flags & ~CELL_WIDE) | CELL_WIDE_CONT;
        shadow[1]  = cont;
    }
    rs->cursor_row = r;
    rs->cursor_col = c + ((cell->flags & CELL_WIDE) ? 2 : 1);
}

/* Does row r of s continue on the row below, so that the terminal must
 * wrap it?  (The last row of the screen never does: its wrap would scroll.) */
static bool row_wants_wrap(const Screen *s, int r, int rows, int cols)
{
    return cols >= 2 && r + 1 < rows && s->row_flags &&
           (s->row_flags[r] & ROW_WRAPPED);
}

/* The cell that ends a wrapped row: its last column, or the lead cell of a
 * wide character that fills the last two columns. */
static int wrap_cell_col(const Screen *s, int r, int cols)
{
    int lc = cols - 1;
    if (s->cells[r][lc].flags & CELL_WIDE_CONT) lc--;
    return lc;
}

/* Does the terminal's wrap flag for any row differ from what s says? */
static bool flags_differ(const RenderState *rs, const Screen *s, int rows, int cols)
{
    for (int r = 0; r < rows; r++)
        if (row_wants_wrap(s, r, rows, cols) != rs->shadow_row_wrapped[r])
            return true;
    return false;
}

/*
 * Take the wrap flag off row r, which the screen no longer wants wrapped.
 * An erase to the end of the line from column 0 clears the flag on the
 * terminal (EL 0; EL 2 does not, on xterm).  The row is then drawn afresh,
 * so the shadow is blank.
 */
static void clear_row_flag(RenderState *rs, int r, int cols)
{
    emit_move(rs, r, 0);
    emit_sgr(rs, COLOR_DEFAULT, COLOR_DEFAULT, 0);
    out_str("\033[K");
    for (int c = 0; c < cols; c++)
        rs->shadow[r][c] = BLANK_CELL;
    rs->shadow_row_wrapped[r] = false;
}

/*
 * Draw one row, sending the cells that differ from the shadow.
 *
 * wrapped: the row continues on the row below.  The terminal wraps only when
 * a character is printed after the last column, so the cell that ends the
 * row is printed, and then the first cell of the row below is printed at
 * once.  That character is what wraps the terminal and sets the wrap flag of
 * this row.  force_c is a cell to print even though it is unchanged: the
 * wrap cell, when this row's flag is not yet on the terminal.  nrow and nsrow
 * are the row below and its shadow (used only when wrapped).
 *
 * Returns true when the row's wrap was made on the terminal.
 */
static bool draw_row(RenderState *rs, const Cell *crow, Cell *srow, int r,
                     int cols, bool wrapped, int force_c,
                     const Cell *nrow, Cell *nsrow)
{
    /* The blank tail of this row: from tail on, nothing is shown. */
    int tail = cols;
    while (tail > 0 && cell_is_blank(&crow[tail - 1])) tail--;
    bool made = false;

    for (int c = 0; c < cols; c++) {
        const Cell *cell   = &crow[c];
        Cell       *shadow = &srow[c];

        /* Skip wide-char continuation cells – emitting the lead
         * cell already advanced the real cursor by two columns. */
        if (cell->flags & CELL_WIDE_CONT) {
            *shadow = *cell;
            continue;
        }

        if (c != force_c && cells_eq(cell, shadow)) continue;

        /* The rest of the row is blank and differs: erase to the end of
         * the line (3 bytes) rather than send blanks for it.  The erase
         * takes the current background, so the SGR is reset first.  The
         * cursor does not move, and the rest of the row is done.  Not on a
         * wrapped row, whose wrap cell must be printed. */
        if (!wrapped && c >= tail && cols - c > 3) {
            emit_move(rs, r, c);
            emit_sgr(rs, COLOR_DEFAULT, COLOR_DEFAULT, 0);
            out_str("\033[K");
            for (int k = c; k < cols; k++)
                srow[k] = crow[k];
            break;
        }

        /* A short gap since the last byte written on this row, of
         * cells that are blank and need no change: write those blanks,
         * rather than a cursor move to get past them.  Only with the
         * SGR already default, so the blanks cost no SGR change. */
        if (rs->cursor_row == r && rs->cursor_col >= 0
                && rs->cursor_col < c && c - rs->cursor_col <= GAP_FILL_MAX
                && rs->cur_fg == COLOR_DEFAULT && rs->cur_bg == COLOR_DEFAULT
                && rs->cur_attrs == 0) {
            bool gap_blank = true;
            for (int k = rs->cursor_col; k < c && gap_blank; k++)
                gap_blank = cell_is_blank(&crow[k]);
            if (gap_blank) {
                while (rs->cursor_col < c) {
                    out_char(' ');
                    rs->cursor_col++;
                }
            }
        }

        put_cell(rs, r, c, cell, shadow, cols, true);

        if (rs->cursor_col >= cols) {
            if (wrapped) {
                /* The terminal is now waiting to wrap.  Print the first cell
                 * of the row below: it wraps, and the flag goes on this row. */
                put_cell(rs, r + 1, 0, &nrow[0], &nsrow[0], cols, false);
                made = true;
            } else {
                rs->cursor_col = -2; /* not known: force explicit moves */
            }
        }
    }
    return made;
}

void render_screen(RenderState *rs, const Screen *s)
{
#ifdef X11_BACKEND
    (void)rs;
    xscr_render(s);
    return;
#endif
    int rows = s->rows < rs->rows ? s->rows : rs->rows;
    int cols = s->cols < rs->cols ? s->cols : rs->cols;

    /* Fast path: if the cursor is already parked correctly (a hidden one
     * has no place to be) and nothing in the cell grid has changed, emit
     * nothing at all. */
    int changed  = count_changed(rs, s, rows, cols, 0);
    int status_n = status_changed(rs);
    bool cursor_ok = rs->cursor_visible == s->cur_visible &&
                     (!s->cur_visible ||
                      (rs->cursor_row == s->cur_row && rs->cursor_col == s->cur_col));
    if (!changed && !status_n && cursor_ok && !flags_differ(rs, s, rows, cols))
        return;

    changed = maybe_scroll(rs, s, rows, cols, changed);

    /* A frame that rewrites a lot is hidden from view while it is drawn, so
     * that the cursor is not seen running about: 12 bytes, worth it only
     * when the frame is big.  A small one is a single write and shows no
     * flicker, and hiding the cursor just to park it would cost more than
     * the park itself. */
    if (rs->cursor_visible && changed + status_n >= HIDE_MIN_CELLS) {
        out_str("\033[?25l");
        rs->cursor_visible = false;
    }

    /* Each row: its wrap flag is brought into line with the screen's (a
     * stale flag is cleared, a missing one is made by the wrap in draw_row),
     * then the row is drawn. */
    for (int r = 0; r < rows; r++) {
        bool want = row_wants_wrap(s, r, rows, cols);
        if (rs->shadow_row_wrapped[r] && !want)
            clear_row_flag(rs, r, cols);
        int force_c = (want && !rs->shadow_row_wrapped[r])
                          ? wrap_cell_col(s, r, cols) : -1;
        if (draw_row(rs, s->cells[r], rs->shadow[r], r, cols, want, force_c,
                     want ? s->cells[r + 1] : NULL,
                     want ? rs->shadow[r + 1] : NULL))
            rs->shadow_row_wrapped[r] = true;
    }

    /* The status row, below the screen: the same diff as any other row. */
    if (status_n)
        draw_row(rs, rs->status, rs->shadow[rs->rows], rs->status_row,
                 rs->cols, false, -1, NULL, NULL);

    /* Restore SGR to default before placing cursor */
    emit_sgr(rs, COLOR_DEFAULT, COLOR_DEFAULT, 0);

    /*
     * Park the cursor at the virtual position, and show it if it is to be
     * shown.  A cursor that is to stay hidden is not moved: where it sits
     * does not matter, and the renderer knows where it is.
     */
    if (s->cur_visible)
        emit_move(rs, s->cur_row, s->cur_col);
    if (s->cur_visible != rs->cursor_visible) {
        out_str(s->cur_visible ? "\033[?25h" : "\033[?25l");
        rs->cursor_visible = s->cur_visible;
    }

    out_flush();
}

void render_full_redraw(RenderState *rs, const Screen *s)
{
#ifdef X11_BACKEND
    (void)rs;
    xscr_render(s);
    return;
#endif
    /* Clear wrap-flag shadow: all rows need wrap flag re-established */
    if (rs->shadow_row_wrapped)
        memset(rs->shadow_row_wrapped, 0,
               (size_t)rs->rows * sizeof(bool));

    /* Erase in the default colours: the erase takes the current background
     * with it, so the SGR state is reset first, and recorded as reset. */
    out_str("\033[m\033[2J\033[H");
    out_flush();
    rs->cur_fg    = COLOR_DEFAULT;
    rs->cur_bg    = COLOR_DEFAULT;
    rs->cur_attrs = 0;

    /* The real cursor is now at (0,0). */
    rs->cursor_row = 0;
    rs->cursor_col = 0;

    /* The erase left every cell blank, so the shadow says so, and the diff
     * sends only the cells of s that are not blank. */
    for (int r = 0; r <= rs->rows; r++)         /* (the status row too) */
        for (int c = 0; c < rs->cols; c++)
            rs->shadow[r][c] = BLANK_CELL;

    render_screen(rs, s);
}

void render_invalidate_cursor(RenderState *rs)
{
    rs->cursor_row = -1;
    rs->cursor_col = -1;
}
