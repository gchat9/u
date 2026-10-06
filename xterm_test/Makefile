# Test and benchmark programs for _lib/xterm.c (see README).
# `make` builds both; they link against xterm.c the same way xterm_demo does.

ifndef TARGET
all clean:
	$(MAKE) TARGET=xt_test $@
	$(MAKE) TARGET=xt_bench $@
.PHONY: all clean
else
RW := 1
EXTRA_OBJS := xterm.o
DEPS := ../_lib/xterm.h
include ../_mk/common.mk

xterm.o: ../_lib/xterm.c ../_lib/xterm.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -c -o $@ ../_lib/xterm.c
endif
