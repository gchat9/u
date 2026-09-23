#include "x11_backend.h"

#include "../xterm_demo/xterm_lib.h"

#include <stdint.h>
#include <string.h>

int x11_backend_init(void)
{
    return xterm_init();
}

int x11_backend_rows(void) { return xterm_rows(); }
int x11_backend_columns(void) { return xterm_columns(); }
int x11_backend_wait(int timeout_ms) { return xterm_wait(timeout_ms); }
int x11_backend_fd(void) { return xterm_fd(); }
int x11_backend_read_input(uint8_t *buf, int cap) { return xterm_read_key(buf, cap); }

void x11_backend_render(const Screen *s)
{
    uint8_t *fb = xterm_framebuffer();
    int cols = xterm_columns();
    int rows = xterm_rows();
    int h = s->rows < rows ? s->rows : rows;
    int w = s->cols < cols ? s->cols : cols;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            const Cell *cell = &s->cells[r][c];
            uint32_t ch = cell->ch;
            fb[r * cols + c] = (ch >= 0x20 && ch < 0x80) ? (uint8_t)ch :
                                (ch == 0 ? ' ' : '?');
        }
        for (int c = w; c < cols; c++) fb[r * cols + c] = ' ';
    }
    xterm_render();
}

void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    (void)active; (void)child_pids; (void)wins_alive;
    uint8_t *fb = xterm_framebuffer();
    int xcols = xterm_columns();
    int xrows = xterm_rows();
    if (row < 0 || row >= xrows) return;
    if (cols > xcols) cols = xcols;
    for (int c = 0; c < cols; c++) fb[row * xcols + c] = ' ';
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
