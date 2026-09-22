TARGET := xterm_demo
RW := 1
EXTRA_OBJS := xterm_lib.o
DEPS := xterm_lib.h
include ../_mk/common.mk

xterm_lib.o: xterm_lib.c xterm_lib.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -c -o $@ xterm_lib.c
