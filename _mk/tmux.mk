# Shared by tmux/ and xtmux/ (the X11 build): the same program, built from
# the same sources. Each directory's Makefile sets the variables below, then
# includes this; anything else it needs (flags, extra rules) it adds after.
#
#   TARGET       the binary, named after the directory
#   EXTRA_OBJS   objects beyond SRCS (xtmux: its X11 backend)
#
# SRCS is the one list of sources the two builds share. xtmux compiles them
# from ../tmux into its own directory, so each build's objects stay apart.

CC      := diet gcc
CFLAGS  = -Wall -Wextra -Wno-unused-result -g -O2 -D_XOPEN_SOURCE=700 -D_GNU_SOURCE
LDFLAGS = -lutil -static

SRCS = main.c pty.c vt.c render.c input.c status.c session.c
OBJS = $(SRCS:.c=.o) $(EXTRA_OBJS)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

# -MMD: write a .d file listing header dependencies for this .o
# -MP:  add phony targets for each header so a deleted header doesn't
#       cause "No rule to make target" errors
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Pull in the generated dependency files (if they exist yet)
-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all clean
