#include <fcntl.h>
#include <string.h>

#include "xtty.h"
#include "xterm_screen.h"

static VTParser vt;
static uint8_t  dirty, resized;
static int      mouse_level;

int xtty_open(const char *title)
{
    if (xterm_init()) return -1;
    fcntl(xterm_fd(), F_SETFD, FD_CLOEXEC);   /* not for child processes: they would keep the window alive */
    xterm_set_title(title, (int)strlen(title));
    vt_init(&vt, xterm_rows(), xterm_columns());
    dirty = 1;
    return 0;
}

int  xtty_fd(void) { return xterm_fd(); }
int  xtty_closed(void) { return xterm_close_requested() || xterm_lost(); }
void xtty_close(void) { vt_free(&vt); xterm_disconnect(); }
void xtty_size(int *rows, int *cols) { *rows = vt.scr.rows; *cols = vt.scr.cols; }
int  xtty_resized(void) { int r = resized; resized = 0; return r; }

void xtty_write(const void *buf, size_t n) { vt_feed(&vt, buf, n); dirty = 1; }

void xtty_flush(void)
{
    if (dirty) { dirty = 0; xscr_render(&vt.scr); }
}

/* "ESC [ row ; col R", the reply to a cursor position query. */
static void reply_cpr(xtty_input_fn input, void *ctx)
{
    uint8_t r[16], *p = r + sizeof r;
    *--p = 'R';
    for (int v = vt.scr.cur_col + 1; v; v /= 10) *--p = (uint8_t)('0' + v % 10);
    *--p = ';';
    for (int v = vt.scr.cur_row + 1; v; v /= 10) *--p = (uint8_t)('0' + v % 10);
    *--p = '[';
    *--p = 27;
    input(p, (size_t)(r + sizeof r - p), ctx);
}

void xtty_poll(int timeout_ms, xtty_input_fn input, void *ctx)
{
    const Screen *s = &vt.scr;
    xtty_flush();
    if (xterm_wait(timeout_ms)) {            /* exposed or resized */
        if (xterm_rows() != s->rows || xterm_columns() != s->cols) {
            vt_resize(&vt, xterm_rows(), xterm_columns());
            resized = 1;
        }
        dirty = 1;
    }
    uint8_t k[64];
    for (int n; (n = xterm_read_key(k, sizeof k)) > 0; ) {
        if (xscr_selection_drop()) dirty = 1;       /* typing ends a selection */
        input(k, (size_t)n, ctx);
    }
    int want = s->mouse_mode < 2 ? 2 : s->mouse_mode;   /* the mouse is also ours */
    if (want != mouse_level) xterm_mouse_select(mouse_level = want);
    if (xterm_paste_key()) xscr_paste(s->bracketed_paste, input, ctx);
    xscr_pointer(s, s->mouse_mode, s->mouse_enc, s->bracketed_paste, s->rows, s->cols, input, ctx);
    if (vt.cpr_requested) { vt.cpr_requested = 0; reply_cpr(input, ctx); }
    xtty_flush();
}
