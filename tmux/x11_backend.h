#pragma once

#include "vt.h"
#include <sys/types.h>

int  x11_backend_init(void);
int  x11_backend_rows(void);
int  x11_backend_columns(void);
int  x11_backend_wait(int timeout_ms);
int  x11_backend_fd(void);
int  x11_backend_read_input(uint8_t *buf, int cap);
/* Draws the screen's content and, from the same Screen, keeps the
 * full-block cursor (no blink) current — see xterm_lib's xterm_set_cursor
 * for why this is folded in here rather than being a separate call. */
void x11_backend_render(const Screen *screen);
/* Nonzero once the window manager has asked us to close (WM_DELETE_WINDOW),
 * e.g. the user clicked the window's close button. Sticky. */
int  x11_backend_close_requested(void);
void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[]);
