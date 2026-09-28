CC      := diet gcc
CFLAGS  = -Wall -Wextra -Wno-unused-result -g -O2 -D_XOPEN_SOURCE=700 -D_GNU_SOURCE
LDFLAGS = -lutil -static

# Two targets, built from two entirely separate sets of object files:
#   tmux   plain VT-to-VT build
#   xtmux  X11 build (X11_BACKEND), plus x11_backend.c and xterm_lib.c
# The shared sources (main.c, vt.c, ...) compile differently in each
# mode (#ifdef X11_BACKEND), so the X11 objects use a distinct .x11.o
# suffix rather than reusing plain .o files under a mode toggle -- that
# used to mean building one target after the other could silently link
# a stale mix of plain and X11 objects, or overwrite the other's final
# binary outright. With separate names, both builds' outputs sit on
# disk at once and `make` never has to guess which mode an existing
# .o belongs to.
SRCS      = main.c pty.c vt.c render.c input.c status.c session.c
OBJS      = $(SRCS:.c=.o)
X11_OBJS  = $(SRCS:.c=.x11.o) x11_backend.x11.o xterm_lib.x11.o
DEPS      = $(SRCS:.c=.d) $(SRCS:.c=.x11.d) x11_backend.x11.d

all: tmux xtmux

tmux: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

xtmux: $(X11_OBJS)
	$(CC) $(X11_OBJS) $(LDFLAGS) -o $@

# -MMD: write a .d file listing header dependencies for this .o
# -MP:  add phony targets for each header so a deleted header doesn't
#       cause "No rule to make target" errors
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.x11.o: %.c
	$(CC) $(CFLAGS) -DX11_BACKEND -MMD -MP -c $< -o $@

xterm_lib.x11.o: ../xterm_demo/xterm_lib.c ../xterm_demo/xterm_lib.h
	$(CC) $(CFLAGS) -DX11_BACKEND -DXTERM_LIB_ONLY -c $< -o $@

# Pull in the generated dependency files (if they exist yet)
-include $(DEPS)

clean:
	rm -f $(OBJS) $(X11_OBJS) $(DEPS) tmux xtmux

.PHONY: all clean
