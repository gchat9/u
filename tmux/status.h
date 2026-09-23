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

/* Render and write directly to STDOUT_FILENO. */
void status_draw(int rows, int cols,
                 int active,
                 pid_t child_pids[],
                 bool  wins_exist[],
                 bool  wins_alive[]);
