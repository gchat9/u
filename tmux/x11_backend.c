#include "x11_backend.h"

#include "../xterm_demo/xterm_lib.h"

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

/* Resolve a Cell's colour down to two xterm-256-palette index bytes.
 * CELL_FG_DFL/CELL_BG_DFL -> 15/0 (this backend's default fg/bg, see
 * xterm_lib.c's build_palette). ATTR_BOLD brightens an 0-7 fg into its
 * 8-15 counterpart (there's no separate bold glyph, so colour is the
 * only bold cue available). ATTR_REVERSE swaps the pair last, matching
 * how a real terminal composites reverse video. Other attrs (dim,
 * italic, underline, strike, invis) aren't rendered by this backend yet. */
static void resolve_colors(const Cell *cell, uint8_t *out_fg, uint8_t *out_bg)
{
    uint8_t fg = (cell->flags & CELL_FG_DFL) ? 15 : cell->fg;
    uint8_t bg = (cell->flags & CELL_BG_DFL) ? 0  : cell->bg;
    if ((cell->attrs & ATTR_BOLD) && fg < 8) fg = (uint8_t)(fg + 8);
    if (cell->attrs & ATTR_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
    *out_fg = fg; *out_bg = bg;
}

void x11_backend_render(const Screen *s)
{
    uint8_t *fb  = xterm_framebuffer();
    uint8_t *fgb = xterm_fg_buffer();
    uint8_t *bgb = xterm_bg_buffer();
    int cols = xterm_columns();
    int rows = xterm_rows();
    int h = s->rows < rows ? s->rows : rows;
    int w = s->cols < cols ? s->cols : cols;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            const Cell *cell = &s->cells[r][c];
            uint32_t ch = cell->ch;
            int i = r * cols + c;
            fb[i] = (ch >= 0x20 && ch < 0x80) ? (uint8_t)ch :
                    (ch == 0 ? ' ' : '?');
            resolve_colors(cell, &fgb[i], &bgb[i]);
        }
        for (int c = w; c < cols; c++) {
            int i = r * cols + c;
            fb[i] = ' '; fgb[i] = 15; bgb[i] = 0;
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

void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    (void)active; (void)child_pids; (void)wins_alive;
    uint8_t *fb  = xterm_framebuffer();
    uint8_t *fgb = xterm_fg_buffer();
    uint8_t *bgb = xterm_bg_buffer();
    int xcols = xterm_columns();
    int xrows = xterm_rows();
    if (row < 0 || row >= xrows) return;
    if (cols > xcols) cols = xcols;
    for (int c = 0; c < cols; c++) {
        int i = row * xcols + c;
        fb[i] = ' '; fgb[i] = 15; bgb[i] = 0;
    }
    int p = 0;
    for (int i = 0; i < 10 && p + 4 < cols; i++) {
        if (!wins_exist[i]) continue;
        fb[row * xcols + p++] = ' ';
        fb[row * xcols + p++] = (uint8_t)('1' + i);
        fb[row * xcols + p++] = ':';
        fb[row * xcols + p++] = ' ';
    }
    xterm_render();
}
