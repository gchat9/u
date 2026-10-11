#pragma once

#include "../_lib/vt.h"
#include "../_lib/xterm_screen.h"
#include "../tmux/config.h"   /* Style */
#include <sys/types.h>

int  x11_backend_init(void);
int  x11_backend_rows(void);
int  x11_backend_columns(void);
int  x11_backend_wait(int timeout_ms);
int  x11_backend_fd(void);
int  x11_backend_read_input(uint8_t *buf, int cap);
/* Nonzero once the window manager has asked us to close (WM_DELETE_WINDOW),
 * e.g. the user clicked the window's close button. Sticky. */
int  x11_backend_close_requested(void);
/* Nonzero once the connection to the X server has been lost (it exited,
 * the network dropped).  Sticky; everything else becomes a no-op. */
int  x11_backend_lost(void);
/* Close the connection, so the window goes away.  Needed before fork()ing
 * the detached daemon, which would otherwise keep the window on screen. */
void x11_backend_shutdown(void);
void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[]);

/* Plain-text view, used by the scrollback viewer: UTF-8 drawn into the
 * window like output to a terminal.  _begin blanks the screen and homes
 * the text cursor; _line writes a line there (wrapping, scrolling the
 * screen up when it passes the bottom); _fill / _at draw in a fixed
 * place (clipped, no wrap), for the footer; _end hides the cursor and
 * shows the result. */
void x11_backend_text_begin(void);
void x11_backend_text_line(const char *utf8, int n, Style st);
void x11_backend_text_fill(int row, Style st);
void x11_backend_text_at(int row, int col, const char *utf8, int n, Style st);
void x11_backend_text_end(void);

/* Mouse and clipboard: the pointer (xscr_pointer, ...) is in
 * ../_lib/xterm_screen.h.  _select says how much pointer traffic is wanted
 * (the displayed program's mouse_mode, 0 none .. 3 all motion; always at
 * least 2 while the mouse is also ours for copy and paste); _paste_key says,
 * once, that Shift+Insert or Ctrl+V asked for a paste. */
void x11_backend_mouse_select(int level);
int  x11_backend_paste_key(void);
