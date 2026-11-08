TARGET := mc
RW     := 1
include ../_mk/common.mk

# The i386 six-argument system call (mmap) takes ebp. With -Os, gcc keeps a frame
# pointer in some functions, which then cannot take it: omit the frame pointer.
CFLAGS += -fomit-frame-pointer

# mc.c includes the other sources (one translation unit, as the libraries here are):
# a change in any of them, or in config.h, rebuilds mc.
mc: $(filter-out mc.c,$(wildcard *.h))

.PHONY: tests
tests: mc
	python3 tests/run.py
