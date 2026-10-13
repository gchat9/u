#include "x11_backend.h"
#include "../tmux/status.h"

#include "../_lib/xterm.h"
#include "../_font/charset.h"
#include "../_font/emoji_charset.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int x11_backend_init(void)
{
    int rc = xterm_init();
    if (rc == 0) {
        static const char title[] = "u-tmux";
        xterm_set_title(title, sizeof title - 1);
    }
    return rc;
}

int x11_backend_rows(void) { return xterm_rows(); }
int x11_backend_columns(void) { return xterm_columns(); }
int x11_backend_wait(int timeout_ms) { return xterm_wait(timeout_ms); }
int x11_backend_fd(void) { return xterm_fd(); }
int x11_backend_read_input(uint8_t *buf, int cap) { return xterm_read_key(buf, cap); }
int x11_backend_close_requested(void) { return xterm_close_requested(); }
int x11_backend_lost(void) { return xterm_lost(); }
void x11_backend_shutdown(void) { xterm_disconnect(); }
void x11_backend_mouse_select(int level) { xterm_mouse_select(level); }
/* Nonzero (once) after a paste shortcut was pressed. */
int x11_backend_paste_key(void) { return xterm_paste_key(); }

/* Context for x11_emit: tracks where the next piece of text lands. */
typedef struct {
    uint8_t *fb, *fgb, *bgb, *atb;
    int row, xcols, cols, col;
} X11StatusCtx;

/* X11 emit: write each byte's glyph + the piece's own fg/bg straight
 * into the per-cell buffers. Style already IS the two bytes those
 * buffers want (see config.h), so there's nothing to parse or convert
 * -- this is the zero-conversion half promised there. */
static void x11_emit(void *vctx, const char *s, int n, Style style)
{
    X11StatusCtx *ctx = vctx;
    for (int i = 0; i < n && ctx->col < ctx->cols; i++, ctx->col++) {
        int idx = ctx->row * ctx->xcols + ctx->col;
        int g = xscr_glyph_slot((uint8_t)s[i]);
        ctx->fb[idx]  = g < 0 ? 0 : (uint8_t)g;
        ctx->fgb[idx] = style.fg;
        ctx->bgb[idx] = style.bg;
        ctx->atb[idx] = g < 0 ? XTERM_ATTR_MISSING : 0;
    }
}

void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    int xcols = xterm_columns();
    int xrows = xterm_rows();
    if (row < 0 || row >= xrows) return;
    if (cols > xcols) cols = xcols;

    X11StatusCtx ctx = {
        .fb = xterm_framebuffer(), .fgb = xterm_fg_buffer(),
        .bgb = xterm_bg_buffer(),  .atb = xterm_attr_buffer(),
        .row = row, .xcols = xcols, .cols = cols, .col = 0,
    };

    /* Blank the row first: status_layout()'s own padding math already
     * covers exactly `cols` columns when content fits, but this is a
     * cheap defensive backstop (e.g. the pathological content-wider-
     * than-cols case) rather than relying on that invariant alone. */
    for (int c = 0; c < cols; c++) {
        int i = row * xcols + c;
        ctx.fb[i] = (uint8_t)xscr_glyph_slot(0); ctx.fgb[i] = XTERM_FG_DEFAULT; ctx.bgb[i] = XTERM_BG_DEFAULT; ctx.atb[i] = 0;
    }

    status_layout(x11_emit, &ctx, cols, active, child_pids, wins_exist, wins_alive);
    xterm_render();
}

/* ── Plain-text view (the scrollback viewer) ──────────────────────────
 * The scrollback is text, not the program's cells, but it is laid out in a
 * Screen (vt.h) and drawn from that with xscr_render(), so xterm_screen.c
 * can select and copy from it exactly as from a program's screen: the
 * same cell grid, wrap flags and wide-character marks, see tv_* below.
 * x11_backend_text_screen() exposes it to xscr_pointer().
 *
 * These functions behave like writing to a terminal: x11_backend_text_line()
 * appends at a text cursor, wraps long lines and scrolls the whole screen
 * up when it runs off the bottom, exactly as the plain build's output to a
 * real terminal does.  Colours come per call, as palette indices.  The
 * footer (the last row) is laid out in the same grid, but the viewer's
 * pointer is kept to the rows above it (see main.c). */

static Screen tv;                 /* the viewer's grid; cur_row -1: no cursor */
static Cell  *tv_store;           /* the cells tv.cells[] point into */
static int    tr_row, tr_col;     /* the text cursor */

/* Make tv match the window's size.  It is blanked by _begin, and a resize
 * redraws the viewer, so nothing needs carrying over.  On allocation
 * failure tv.cells stays NULL and every function here does nothing. */
static void tv_fit(void)
{
    int rows = xterm_rows(), cols = xterm_columns();
    if (tv.cells && tv.rows == rows && tv.cols == cols) return;
    free(tv.cells); free(tv_store); free(tv.row_flags);
    tv.cells = NULL; tv_store = NULL; tv.row_flags = NULL; tv.rows = tv.cols = 0;
    tv.cells     = malloc(sizeof *tv.cells * (size_t)rows);
    tv_store     = malloc(sizeof *tv_store * (size_t)rows * (size_t)cols);
    tv.row_flags = malloc((size_t)rows);
    if (!tv.cells || !tv_store || !tv.row_flags) {
        free(tv.cells); free(tv_store); free(tv.row_flags);
        tv.cells = NULL; tv_store = NULL; tv.row_flags = NULL;
        return;
    }
    for (int r = 0; r < rows; r++) tv.cells[r] = tv_store + (size_t)r * cols;
    tv.rows = rows; tv.cols = cols;
    tv.cur_row = tv.cur_col = -1;
}

static void blank_row(int r, Style st)
{
    for (int c = 0; c < tv.cols; c++) {
        Cell *x = &tv.cells[r][c];
        x->ch = 0; x->fg = st.fg; x->bg = st.bg; x->attrs = 0; x->flags = 0;
    }
    tv.row_flags[r] = 0;
}

void x11_backend_text_fill(int row, Style st)
{
    if (tv.cells && row >= 0 && row < tv.rows) blank_row(row, st);
}

void x11_backend_text_begin(void)
{
    tv_fit();
    if (!tv.cells) return;
    for (int r = 0; r < tv.rows; r++) blank_row(r, (Style){ XTERM_FG_DEFAULT, XTERM_BG_DEFAULT });
    tr_row = tr_col = 0;
}

/* Next UTF-8 character of [*p, end); tolerant of truncated or stray
 * bytes (a stray byte is returned as itself).  Zero-width characters
 * (see vt.c's put_char) come back as 0 and take no cell. */
static uint32_t next_char(const char **p, const char *end, int *width)
{
    const unsigned char *u = (const unsigned char *)*p;
    uint32_t cp = u[0]; int len = 1;
    if      (cp >= 0xF0) { cp &= 0x07; len = 4; }
    else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
    else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
    if (len > end - *p) len = 1, cp = u[0];
    for (int k = 1; k < len; k++) cp = cp << 6 | (u[k] & 0x3F);
    *p += len;
    *width = (cp >= 0x1100 && (emoji_wide(cp) || charset_wide(cp))) ? 2 : 1;
    if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0x200B && cp <= 0x200F)) return 0;
    return cp;
}

/* Store cp at (row, col) in the grid, `width` cells wide.  A wide character
 * is a CELL_WIDE cell and a CELL_WIDE_CONT cell, which xscr_render draws as
 * the two halves of one glyph; the continuation's ch stays 0 so that, if
 * it is ever left without its partner, it is drawn blank. */
static void put_text_char(int row, int col, uint32_t cp, int width, Style st)
{
    for (int h = 0; h < width; h++) {
        Cell *x = &tv.cells[row][col + h];
        x->ch    = h ? 0 : cp;
        x->fg    = st.fg;
        x->bg    = st.bg;
        x->attrs = 0;
        x->flags = width == 2 ? (h ? CELL_WIDE_CONT : CELL_WIDE) : 0;
    }
}

/* Draw up to n bytes of text at (row, col), clipped to the row, no wrap. */
void x11_backend_text_at(int row, int col, const char *s, int n, Style st)
{
    if (!tv.cells || row < 0 || row >= tv.rows) return;
    for (const char *end = s + n; s < end; ) {
        int w; uint32_t cp = next_char(&s, end, &w);
        if (!cp) continue;
        if (col + w > tv.cols) break;
        put_text_char(row, col, cp, w, st);
        col += w;
    }
}

/* End the current row, then move to the next, scrolling the screen up by one
 * row if that runs off the bottom.  wrapped: the line goes on in the next row
 * (what ROW_WRAPPED records, so that a copy joins the two without a newline). */
static void text_break(int wrapped)
{
    if (wrapped) tv.row_flags[tr_row] |= ROW_WRAPPED;
    tr_col = 0;
    if (++tr_row < tv.rows) return;
    tr_row = tv.rows - 1;
    Cell *top = tv.cells[0];
    memmove(tv.cells, tv.cells + 1, sizeof *tv.cells * (size_t)(tv.rows - 1));
    tv.cells[tv.rows - 1] = top;
    memmove(tv.row_flags, tv.row_flags + 1, (size_t)(tv.rows - 1));
    blank_row(tv.rows - 1, (Style){ XTERM_FG_DEFAULT, XTERM_BG_DEFAULT });
}

void x11_backend_text_line(const char *s, int n, Style st, int wrapped)
{
    if (!tv.cells) return;
    for (const char *end = s + n; s < end; ) {
        int w; uint32_t cp = next_char(&s, end, &w);
        if (!cp) continue;
        if (w > tv.cols) continue;            /* would not fit on a row */
        if (tr_col + w > tv.cols) text_break(1);
        put_text_char(tr_row, tr_col, cp, w, st);
        tr_col += w;
    }
    text_break(wrapped);                      /* wrapped: the next line continues it */
}

/* Show what was drawn above, with the selection highlighted if there is
 * one.  The text view has no cursor (cur_row is -1). */
void x11_backend_text_end(void)
{
    if (tv.cells) xscr_render(&tv);
}

const Screen *x11_backend_text_screen(void) { return &tv; }
