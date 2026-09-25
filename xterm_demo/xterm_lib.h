#ifndef XTERM_LIB_H
#define XTERM_LIB_H

uint8_t *xterm_framebuffer(void);
int xterm_columns(void);
int xterm_rows(void);
int xterm_init(void);
void xterm_render(void);
int xterm_wait(int timeout_ms);

/* The underlying X11 connection socket, for callers that want to select()/
 * poll() on it themselves alongside other fds (rather than only calling
 * xterm_wait() on a fixed schedule). */
int xterm_fd(void);

/* Drain up to `cap` bytes of keyboard input translated from KeyPress events
 * (plain ASCII for printable keys, Ctrl-letter control codes, and a handful
 * of ANSI escape sequences for arrows/backspace/etc) into `buf`. Events are
 * queued as a side effect of xterm_wait(); call this after xterm_wait()
 * reports activity to retrieve what it queued. Returns the number of bytes
 * written (0 if none pending). */
int xterm_read_key(uint8_t *buf, int cap);

/* Cursor: full-block fg/bg inversion at (row,col) on the next
 * xterm_render(). No blinking. Pass row<0 to hide it. */
void xterm_set_cursor(int row, int col);

/* Set WM_NAME (window title / taskbar label). */
void xterm_set_title(const char *name, int len);

/* Nonzero once the window manager has sent a WM_DELETE_WINDOW request
 * (the user closed the window). Sticky — stays set once seen. */
int xterm_close_requested(void);

#endif
