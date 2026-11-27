# Shared by mssh/ and xmssh/ (the X11 build): the same SSH client, built from
# the same sources, linked with a different terminal front-end (tty.c). Each
# directory's Makefile sets the variables below, then includes this; anything
# else it needs (flags, extra rules) it adds after.
#
#   TARGET       the binary, named after the directory
#   SRCS         sources; found in this directory, else in ../mssh (so xmssh/
#                has mssh.c from there and a tty.c of its own)
#   EXTRA_OBJS   objects beyond SRCS (xmssh: the xterm layer)
#
# Freestanding, as the small tools are (see common.mk): no C library, no libc
# headers; _sys/_.h is the system call layer, and the client's own helpers are
# in _lib/. The cryptography is _lib/sodium_lite: the few libsodium functions
# the client calls, in 650 lines (the client is compiled against its sodium.h
# unchanged). It is checked against the real libsodium by _lib/sodium_lite/
# test.py; the client itself cannot be linked with the system library, which
# needs a C library.

CC      := gcc
# Size is the point of this project: -Os, one section per function and datum
# for --gc-sections, and no stack protector or unwind tables (nothing here
# unwinds), and no frame pointer: the i386 six-argument system calls take ebp,
# see _arch/i386.h. -Wno-attributes: the naked return trampolines, see _arch/.
CFLAGS  = -Wall -Wextra -Wno-unused-result -Wno-attributes -g -Os -ffreestanding \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fomit-frame-pointer \
          -ffunction-sections -fdata-sections -MMD -MP -I../_lib/sodium_lite
ifneq ($(shell $(CC) -dM -E - < /dev/null | egrep __arm__),)
    CFLAGS += -mcpu=cortex-a15 -mthumb
endif
LDSCRIPT := ../_sys/cleanup-re+rw.ld
LDFLAGS  = -static -nostdlib -fno-pic -fno-pie -no-pie \
           -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
           -Wl,--gc-sections -Wl,-T,$(LDSCRIPT)

.DEFAULT_GOAL := $(TARGET)

vpath %.c ../mssh ../_lib/sodium_lite

OBJS := $(SRCS:.c=.o) $(EXTRA_OBJS) sodium_lite.o randombytes.o

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) *.o *.d

-include $(OBJS:.o=.d)
