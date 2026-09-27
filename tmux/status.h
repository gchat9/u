#pragma once

#include <stdbool.h>
#include <sys/types.h>
#include "config.h"  /* MAX_WINDOWS */

/*
 * Render the status bar into buf (escape sequences, not NUL-terminated).
 * Returns the number of bytes written, or -1 if buf is too small.
 */
int status_render(char *buf, size_t buflen,
                  int rows, int cols, int active,
                  pid_t child_pids[], bool wins_exist[], bool wins_alive[]);

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

/* Render and write directly to STDOUT_FILENO. */
void status_draw(int rows, int cols,
                 int active,
                 pid_t child_pids[],
                 bool  wins_exist[],
                 bool  wins_alive[]);
