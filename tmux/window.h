#pragma once

#include <stdbool.h>
#include "pty.h"
#include "vt.h"

typedef struct {
    Pty       pty;
    VTParser  vt;
    bool      alive;
    bool      pristine; /* default shell, no input yet → auto-close on switch-away */
    char    **argv;   /* NULL = default shell; else exec argv (owned copy) */
} Window;
