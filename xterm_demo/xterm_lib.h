#ifndef XTERM_LIB_H
#define XTERM_LIB_H

uint8_t *xterm_framebuffer(void);
int xterm_columns(void);
int xterm_rows(void);
int xterm_init(void);
void xterm_render(void);
int xterm_wait(int timeout_ms);

#endif
