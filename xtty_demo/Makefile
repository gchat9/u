# xtty_demo: a minimal program that talks to its terminal through xtty, see
# ../_lib/xtty.h.  Built like xtmux (dietlibc; vt.c needs malloc).
TARGET     := xtty_demo
SRCS       := xtty_demo.c xtty.c vt.c xterm_screen.c
EXTRA_OBJS := xterm.o
include ../_mk/tmux.mk
LDFLAGS    := -static
CFLAGS     += -DX11_BACKEND

xterm.o: ../_lib/xterm.c ../_lib/xterm.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -MMD -MP -c $< -o $@
