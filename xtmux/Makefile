# xtmux: build of ../tmux with X11_BACKEND defined
# Only x11_backend.* is xtmux-specific; ../_lib/xterm.c (the window) and
# ../_lib/xterm_screen.c (a Screen shown in it) are shared

TARGET     := xtmux
EXTRA_OBJS := x11_backend.o xterm_screen.o xterm.o
include ../_mk/tmux.mk

# vpath for .c files only, deliberately not VPATH: that would also search
# ../tmux for .o files, and we don't want that
vpath %.c ../tmux

# -I.  the shared sources #include "x11_backend.h" (under X11_BACKEND), which
#      lives here. Deliberately no -I../tmux: angle-bracket includes search -I
#      paths too, so tmux's own pty.h would shadow the system <pty.h> that
#      pty.c needs. x11_backend.* name the tmux headers they use explicitly.
CFLAGS += -DX11_BACKEND -I.

xterm.o: ../_lib/xterm.c ../_lib/xterm.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -MMD -MP -c $< -o $@
