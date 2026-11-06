TARGET := mc
RW     := 1
include ../_mk/common.mk

# The i386 six-argument system call (mmap) takes ebp. With -Os, gcc keeps a frame
# pointer in some functions, which then cannot take it: omit the frame pointer.
CFLAGS += -fomit-frame-pointer

# The parameters are in config.h: a change there rebuilds mc.
mc: config.h

.PHONY: tests
tests: mc
	python3 tests/run.py
