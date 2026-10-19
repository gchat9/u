#pragma once

#include <stdbool.h>
#include <sys/types.h>
#include "config.h"  /* MAX_WINDOWS */
#include "../_lib/vt.h"  /* Cell */

/*
 * Shared status-bar content: hostname, per-window tab labels/highlight
 * state (active/dead/normal), clock, and the padding to make it all
 * add up to exactly `cols` columns. This is the single source of truth
 * for *what* the status bar shows, used by both backends -- each just
 * supplies its own `emit`, which is handed each piece of text together
 * with the Style (see config.h) it should be drawn in. `emit` may be
 * called with n=0; implementations should handle that as a no-op.
 */
typedef void (*StatusEmit)(void *ctx, const char *s, int n, Style style);

void status_layout(StatusEmit emit, void *ctx, int cols, int active,
                   pid_t child_pids[], bool wins_exist[], bool wins_alive[]);

/*
 * Fill row (cols cells) with the status bar: the same content, as cells, to
 * be drawn by the renderer (render_status_row), which sends only what
 * changed since the last time.
 */
void status_cells(Cell *row, int cols, int active,
                  pid_t child_pids[], bool wins_exist[], bool wins_alive[]);

/* Write text into row at column x, in style st; returns the column after
 * it.  Anything but printable ASCII is written as '?'. */
int status_put(Cell *row, int cols, int x, const char *s, int n, Style st);

#ifdef X11_BACKEND
/* The X11 build draws the bar itself (x11_backend.c). */
void status_draw(int rows, int cols, int active,
                 pid_t child_pids[], bool wins_exist[], bool wins_alive[]);
#endif
