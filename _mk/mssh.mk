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
# The cryptography is _lib/sodium_lite: the few libsodium functions the client
# calls, in 650 lines (the client is compiled against its sodium.h unchanged).
# For checking against the real thing:  make SODIUM=system   (libsodium-dev;
# glibc, as dietlibc cannot link the system libsodium.a).

SODIUM ?= lite

ifeq ($(SODIUM),system)
CC      := cc
CFLAGS  = -O2 -Wall -Wextra -MMD -MP
LDLIBS  = -lsodium
else
CC      := diet gcc
CFLAGS  = -Wall -Wextra -Wno-unused-result -g -O2 -MMD -MP -I../_lib/sodium_lite
LDFLAGS = -static
SODIUM_OBJS := sodium_lite.o randombytes.o
endif

.DEFAULT_GOAL := $(TARGET)

vpath %.c ../mssh ../_lib/sodium_lite

OBJS := $(SRCS:.c=.o) $(EXTRA_OBJS) $(SODIUM_OBJS)

ifeq ($(SODIUM),system)
# A readable message instead of "sodium.h: No such file" (and `make clean`
# must work without libsodium, so this is only a prerequisite of the build).
.PHONY: check-sodium
check-sodium:
	@echo '#include <sodium.h>' | $(CC) -E -x c - >/dev/null 2>&1 || \
	    { echo "$(TARGET): libsodium headers not found: install libsodium-dev" >&2; exit 1; }
$(OBJS): | check-sodium
endif

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) *.o *.d

-include $(OBJS:.o=.d)
