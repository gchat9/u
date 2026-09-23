CC      := diet gcc
CFLAGS  = -Wall -Wextra -Wno-unused-result -g -O2 -D_XOPEN_SOURCE=700 -D_GNU_SOURCE
LDFLAGS = -lutil -static

SRCS = main.c pty.c vt.c render.c input.c status.c session.c
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)
TARGET = tmux

ifeq ($(X11),1)
CFLAGS += -DX11_BACKEND
OBJS += x11_backend.o xterm_lib.o
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

# -MMD: write a .d file listing header dependencies for this .o
# -MP:  add phony targets for each header so a deleted header doesn't
#       cause "No rule to make target" errors
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

ifeq ($(X11),1)
xterm_lib.o: ../xterm_demo/xterm_lib.c ../xterm_demo/xterm_lib.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -c $< -o $@
endif

# Pull in the generated dependency files (if they exist yet)
-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) x11_backend.o x11_backend.d xterm_lib.o

.PHONY: all clean
