#pragma once
/*
 * xtty -- a terminal emulator window for a program that talks to its
 * terminal itself.  Instead of write(1, ...) and read(0, ...) it uses
 * xtty_write() and xtty_poll(); xtty parses what it is given (vt.c), shows
 * it in an X11 window (xterm.c, xterm_screen.c), and hands back what a
 * terminal would send: keys, mouse reports, pasted text, replies to
 * queries.  Copy and paste, selection, resizing are all inside.
 *
 *   xtty_open("my app");
 *   for (;;) {
 *       ... write the frame with xtty_write() ...
 *       xtty_poll(timeout_ms, my_input, my_state);     // or after poll(2)
 *       if (xtty_closed()) break;
 *       if (xtty_resized()) { xtty_size(&rows, &cols); ... lay out again }
 *   }
 *
 * Needs libc (vt.c allocates).  Build: xtty.c, xterm_screen.c, xterm.c, vt.c
 * with -DX11_BACKEND -DXTERM_LIB_ONLY, as xtmux does; see ../xtty_demo.
 */
#include <stddef.h>
#include <stdint.h>

/* Where input goes: called with the bytes the terminal produces, as many
 * times as needed, from inside xtty_poll(). */
typedef void (*xtty_input_fn)(const uint8_t *bytes, size_t n, void *ctx);

/* Connect to the X server ($DISPLAY) and open the window, with the
 * given title.  0 on success. */
int  xtty_open(const char *title);

/* The X connection, for a poll(2) of your own alongside other fds: when it
 * is readable, call xtty_poll(0, ...). */
int  xtty_fd(void);

/* Terminal output: escape sequences and text, as to any terminal.  Shown at
 * the next xtty_flush() or xtty_poll(). */
void xtty_write(const void *buf, size_t n);
void xtty_flush(void);

/* Handle X events, waiting up to timeout_ms for the first (0: do not
 * wait), and call input() for what they produce.  Flushes first, and
 * again after. */
void xtty_poll(int timeout_ms, xtty_input_fn input, void *ctx);

/* The window's size in cells, and whether it changed since the last call
 * of xtty_resized() (the SIGWINCH of this terminal; the parser already
 * resized its screen, the program has to redraw). */
void xtty_size(int *rows, int *cols);
int  xtty_resized(void);

/* The window was closed, or the X server went away. */
int  xtty_closed(void);
void xtty_close(void);
