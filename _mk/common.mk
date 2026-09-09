# Common build rules for individual tools (pwd/, sleep/, ...).
#
# A tool's Makefile sets a couple of variables and includes this file:
#
#   TARGET := pwd
#   include ../_mk/common.mk
#
# Variables a tool's Makefile may set before the include:
#
#   TARGET     (required) name of the tool. Also its only "primary" source
#              file: $(TARGET).c
#   DEPS       extra prerequisite files the build depends on besides
#              $(TARGET).c - private headers etc. Default: none.
#   RW         set to 1 if the tool has writable globals/statics/bss and
#              so needs a separate read-write PT_LOAD segment. Default:
#              unset, i.e. text + rodata only (smallest possible binary).
#
#              Don't just point LDSCRIPT at cleanup-re+rw.ld by hand: a
#              second, differently-permissioned PT_LOAD segment also needs
#              a real (not sub-page) page-size and file alignment to be
#              mapped correctly by the kernel, which the RW := 1 path
#              below takes care of. Getting only the script right and not
#              the page-size/nmagic to match produces a binary that
#              builds and links cleanly but segfaults at runtime, so this
#              is deliberately one toggle rather than several separate
#              knobs to keep in sync.

_SYS := ../_sys

CFLAGS := -static -nostdlib -flto -Os -fno-stack-clash-protection \
          -fno-stack-protector -fno-exceptions -ffreestanding \
          -ffunction-sections -fdata-sections -Wl,--gc-sections

ifeq ($(RW),1)
LDSCRIPT     := $(_SYS)/cleanup-re+rw.ld
PAGE_SIZE    := 0x1000
NMAGIC_FLAGS :=
else
LDSCRIPT     := $(_SYS)/cleanup-re.ld
PAGE_SIZE    := 0x100
NMAGIC_FLAGS := -Wl,--nmagic
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(TARGET).c $(DEPS)
	$(CC) $(CFLAGS) -static -nostdlib -fno-pic -fno-pie -no-pie \
	  -Wl,-z,max-page-size=$(PAGE_SIZE) \
	  $(NMAGIC_FLAGS) \
	  -Wl,--build-id=none \
	  -Wl,-z,noprop \
	  -Wl,-T,$(LDSCRIPT) \
	  -o $(TARGET) $(TARGET).c

clean:
	rm -f $(TARGET) 2>/dev/null
