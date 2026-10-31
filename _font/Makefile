# Font and charset generation: builds the data files _lib/xterm.c loads
# from /etc at runtime, one pair for each font size --
#
#   font<STEP>.bfnt    the glyphs, laid out as defined in charset.h, and the
#                      colour emoji listed in emoji_charset.h
#
# STEP is 0..7 (SIZES below). Each size's cell is 2^((STEP-2)/4) times the
# size of STEP 2 in both directions: STEP 2 is the size the font has always
# been built at, STEP 6 is twice as large, and STEP 0 and 1 are the two
# smaller sizes below it. xterm starts at STEP 2 (or at the size nearest 30
# lines on the screen) and steps through the others with Ctrl+= and Ctrl+-.
#
# These are host tools producing data, not freestanding tools, so this
# doesn't use _mk/common.mk and (being a _ directory) isn't built by the
# top-level `make`; run `make fonts` there, or `make` here.
#
#   make             build all the files (fetching stb_*.h first if needed)
#   make install     copy them to $(DESTDIR)$(FONTDIR), default /etc/bfnt,
#                    created if need be, as font0 .. font7; the emoji<k> files
#                    of the two-file layout are removed
#   make clean       remove the generator binaries
#   make distclean   clean, plus the downloaded stb_*.h and generated *.bfnt
#
# Inputs from outside the repo: a monospace TTF (TTF=..., default DejaVu
# Sans Mono; Debian/Ubuntu: fonts-dejavu-core), a TTF/TTC for the
# double-width glyphs (WTTF=..., default Noto Sans CJK; Debian/Ubuntu:
# fonts-noto-cjk) and, on first use, network access for stb_*.h. The stb
# headers are pinned to one commit and then
# simply kept: make treats the local file as up to date, so nothing is
# re-downloaded until `make distclean`.

HOSTCC  ?= cc
CFLAGS  ?= -O2
TTF     ?= /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
WTTF    ?= /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
FONTDIR ?= /etc/bfnt

STB_REF ?= 2c980bb59875b0d32144a71867fbdebb2f77cd20
STB_URL  = https://raw.githubusercontent.com/nothings/stb/$(STB_REF)
STB     := stb_image.h stb_truetype.h

SIZES  := 0 1 2 3 4 5 6 7           # keep equal to FONT_SIZES in mkfont.c and xterm.c
FONTS  := $(SIZES:%=font%.bfnt)

all: $(FONTS)

# mkfont writes the glyphs of one size (STEP is the file name's number) to a
# .base file; mkemoji appends that size's emoji to it. mkemoji takes its cell
# size from the .base file, hence the order.
font%.bfnt: mkfont mkemoji $(TTF) $(WTTF) $(wildcard emoji_src/*.png)
	./mkfont $* $@.base $(TTF) $(WTTF)
	./mkemoji $@.base $@
	rm -f $@.base

mkfont: mkfont.c charset.h stb_truetype.h
	$(HOSTCC) $(CFLAGS) -o $@ mkfont.c -lm

mkemoji: mkemoji.c emoji_charset.h ../_lib/width.h stb_image.h
	$(HOSTCC) $(CFLAGS) -o $@ mkemoji.c -lm

$(STB):
	curl -fsSL -o $@ $(STB_URL)/$@

$(TTF):
	@echo "$@: not found. Install a monospace TTF (e.g. fonts-dejavu-core) or pass TTF=/path/to/font.ttf" >&2; false

$(WTTF):
	@echo "$@: not found. Install a font with the wide glyphs (e.g. fonts-noto-cjk) or pass WTTF=/path/to/font.ttc" >&2; false

install: all
	install -d $(DESTDIR)$(FONTDIR)
	for k in $(SIZES); do \
		install -m 644 font$$k.bfnt $(DESTDIR)$(FONTDIR)/font$$k || exit 1; \
		rm -f $(DESTDIR)$(FONTDIR)/emoji$$k; \
	done

clean:
	rm -f mkfont mkemoji

distclean: clean
	rm -f $(STB) *.bfnt *.base

# A failed or interrupted curl must not leave a truncated stb_*.h behind
# for the next run to treat as a valid cached copy.
.DELETE_ON_ERROR:
.PHONY: all install clean distclean
