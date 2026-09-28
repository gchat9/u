#include "x11_backend.h"
#include "status.h"

#include "../xterm_demo/xterm_lib.h"
#include "../xterm_demo/charset.h"

#include <stdint.h>
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

/* Resolve a Cell's colour down to two xterm-256-palette index bytes,
 * plus a small structural-attr bitmask (drawn as extra pixels, not a
 * colour swap — see xterm_lib.c's fill_row).
 * CELL_FG_DFL/CELL_BG_DFL -> 15/0 (this backend's default fg/bg, see
 * xterm_lib.c's build_palette). ATTR_BOLD brightens an 0-7 fg into its
 * 8-15 counterpart (there's no separate bold glyph, so colour is the
 * only bold cue available). ATTR_REVERSE swaps the pair last, matching
 * how a real terminal composites reverse video. ATTR_UNDERLINE and
 * ATTR_STRIKE map straight through to XTERM_ATTR_UNDERLINE/STRIKE.
 * TODO: ATTR_DIM, ATTR_ITALIC, ATTR_BLINK, ATTR_INVIS aren't rendered
 * by this backend yet. */
static void resolve_colors(const Cell *cell, uint8_t *out_fg, uint8_t *out_bg,
                           uint8_t *out_attr)
{
    uint8_t fg = (cell->flags & CELL_FG_DFL) ? 15 : cell->fg;
    uint8_t bg = (cell->flags & CELL_BG_DFL) ? 0  : cell->bg;
    if ((cell->attrs & ATTR_BOLD) && fg < 8) fg = (uint8_t)(fg + 8);
    if (cell->attrs & ATTR_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
    *out_fg = fg; *out_bg = bg;

    uint8_t attr = 0;
    if (cell->attrs & ATTR_UNDERLINE) attr |= XTERM_ATTR_UNDERLINE;
    if (cell->attrs & ATTR_STRIKE)    attr |= XTERM_ATTR_STRIKE;
    *out_attr = attr;
}

/* Maps a decoded Unicode codepoint to this font's glyph slot (see
 * charset.h): ch==0 (an empty/never-written cell) reads as a blank
 * space, and anything the charset has no glyph for falls back to '?'.
 * Neither ' ' nor '?' are valid slot numbers by themselves any more
 * now that the charset isn't identity-mapped -- this is the only
 * place that needs to know that. */
static inline uint8_t glyph_slot(uint32_t ch)
{
    int slot = charset_slot(ch ? ch : ' ');
    if (slot < 0) slot = charset_slot('?');
    return (uint8_t)slot;
}

void x11_backend_render(const Screen *s)
{
    uint8_t *fb  = xterm_framebuffer();
    uint8_t *fgb = xterm_fg_buffer();
    uint8_t *bgb = xterm_bg_buffer();
    uint8_t *atb = xterm_attr_buffer();
    int cols = xterm_columns();
    int rows = xterm_rows();
    int h = s->rows < rows ? s->rows : rows;
    int w = s->cols < cols ? s->cols : cols;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            const Cell *cell = &s->cells[r][c];
            uint32_t ch = cell->ch;
            int i = r * cols + c;
            fb[i] = glyph_slot(ch);
            resolve_colors(cell, &fgb[i], &bgb[i], &atb[i]);
        }
        for (int c = w; c < cols; c++) {
            int i = r * cols + c;
            fb[i] = glyph_slot(0); fgb[i] = 15; bgb[i] = 0; atb[i] = 0;
        }
    }
    /* This is called on every content update (unlike the status bar,
     * which is deliberately redrawn far less often — see main.c), so
     * it's the right place to keep the cursor current: set it from the
     * Screen we were just handed, every time, rather than needing a
     * separate call that could easily end up firing before or after
     * the wrong render and lag a frame behind. */
    xterm_set_cursor(s->cur_row, s->cur_col);
    xterm_render();
}

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
        ctx->fb[idx]  = glyph_slot((uint8_t)s[i]);
        ctx->fgb[idx] = style.fg;
        ctx->bgb[idx] = style.bg;
        ctx->atb[idx] = 0;
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
        ctx.fb[i] = glyph_slot(0); ctx.fgb[i] = 15; ctx.bgb[i] = 0; ctx.atb[i] = 0;
    }

    status_layout(x11_emit, &ctx, cols, active, child_pids, wins_exist, wins_alive);
    xterm_render();
}
