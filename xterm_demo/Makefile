TARGET := xterm_demo
RW := 1
EXTRA_OBJS := xterm.o
DEPS := ../_lib/xterm.h ../_font/charset.h
include ../_mk/common.mk

xterm.o: ../_lib/xterm.c ../_lib/xterm.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -c -o $@ ../_lib/xterm.c
