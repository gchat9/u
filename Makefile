# Top-level build orchestration.
#
#   make                  build every tool in place, using the host CC
#   make x86_64           build every tool for x86_64, sstrip, and collect
#   make i386              the results under _release/<arch>/ (also:
#   make aarch64            aarch64, armhf, riscv64)
#   make armhf
#   make riscv64
#   make release-all      do all of the ARCHES above in one go
#   make release ARCH=foo CC=foo-gcc
#                          same thing, for an arch/cross-compiler not in
#                          the built-in table below
#   make clean             clean every tool's build directory
#   make distclean          clean, and remove _release/ too

TOOLS := $(sort $(patsubst %/Makefile,%,$(wildcard */Makefile)))

ARCHES     := x86_64 i386 aarch64 armhf riscv64
CC_x86_64  := x86_64-linux-gnu-gcc
CC_i386    := i686-linux-gnu-gcc
CC_aarch64 := aarch64-linux-gnu-gcc
CC_armhf   := arm-linux-gnueabihf-gcc
CC_riscv64 := riscv64-linux-gnu-gcc

SSTRIP      ?= sstrip
RELEASE_DIR ?= _release

.PHONY: all clean distclean release release-all $(ARCHES) $(TOOLS)

all: $(TOOLS)

$(TOOLS):
	$(MAKE) -C $@

# `make x86_64` / `make i386` / `make aarch64` / `make armhf` / `make riscv64`
$(ARCHES):
	@$(MAKE) release ARCH=$@ CC=$(CC_$@)

release-all: $(ARCHES)

# `make release ARCH=<name> [CC=<compiler>]` - CC defaults to the host
# compiler if omitted, which is only useful for ARCH names not already
# covered by the $(ARCHES) shortcuts above.
release:
	@test -n "$(ARCH)" || { echo "usage: make release ARCH=<name> [CC=<compiler>]" >&2; exit 1; }
	@mkdir -p $(RELEASE_DIR)/$(ARCH)
	@for t in $(TOOLS); do \
	    $(MAKE) -C $$t clean || exit 1; \
	    $(MAKE) -C $$t CC=$(CC) || exit 1; \
	    cp $$t/$$t $(RELEASE_DIR)/$(ARCH)/$$t; \
	    if command -v $(SSTRIP) >/dev/null 2>&1; then \
	        $(SSTRIP) $(RELEASE_DIR)/$(ARCH)/$$t; \
	    else \
	        echo "warning: '$(SSTRIP)' not found in PATH, leaving $(RELEASE_DIR)/$(ARCH)/$$t un-sstripped" >&2; \
	    fi; \
	done
	@echo "$(RELEASE_DIR)/$(ARCH): $(TOOLS)"

clean:
	@for t in $(TOOLS); do $(MAKE) -C $$t clean; done

distclean: clean
	rm -rf $(RELEASE_DIR)
