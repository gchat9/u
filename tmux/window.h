#pragma once

#include <stdbool.h>
#include "pty.h"
#include "../_lib/vt.h"

typedef struct {
    Pty       pty;
    VTParser  vt;
    bool      alive;
    bool      pristine; /* default shell, no input yet → auto-close on switch-away */
    char    **argv;   /* NULL = default shell; else exec argv (owned copy) */
} Window;

/* Answer the cursor-position request (CSI 6 n) the window's program has made
 * since it was last answered, if any.  Call it whenever the window's output
 * has been read, whether or not the window is on screen (see session.c). */
void window_answer_cpr(Window *w);
