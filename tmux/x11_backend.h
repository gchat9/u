#pragma once

#include "vt.h"
#include <sys/types.h>

int  x11_backend_init(void);
int  x11_backend_rows(void);
int  x11_backend_columns(void);
int  x11_backend_wait(int timeout_ms);
int  x11_backend_fd(void);
int  x11_backend_read_input(uint8_t *buf, int cap);
void x11_backend_render(const Screen *screen);
void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[]);
