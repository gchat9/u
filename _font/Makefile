# Font and charset generation: builds the two data files _lib/xterm.c loads
# from /etc at runtime --
#
#   font.bfnt    monochrome glyphs, laid out as defined in charset.h
#   emoji.bfnt   colour emoji, as listed in emoji_charset.h
#
# These are host tools producing data, not freestanding tools, so this
# doesn't use _mk/common.mk and (being a _ directory) isn't built by the
# top-level `make`; run `make fonts` there, or `make` here.
#
#   make             build both files (fetching stb_*.h first if needed)
#   make install     copy them to $(DESTDIR)$(FONTDIR), default /etc
#   make clean       remove the generator binaries
#   make distclean   clean, plus the downloaded stb_*.h and generated *.bfnt
#
# Inputs from outside the repo: a monospace TTF (TTF=..., default DejaVu
# Sans Mono; Debian/Ubuntu: fonts-dejavu-core) and, on first use, network
# access for stb_*.h. The stb headers are pinned to one commit and then
# simply kept: make treats the local file as up to date, so nothing is
# re-downloaded until `make distclean`.

HOSTCC  ?= cc
CFLAGS  ?= -O2
TTF     ?= /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
FONTDIR ?= /etc

STB_REF ?= 2c980bb59875b0d32144a71867fbdebb2f77cd20
STB_URL  = https://raw.githubusercontent.com/nothings/stb/$(STB_REF)
STB     := stb_image.h stb_truetype.h

all: font.bfnt emoji.bfnt

# mkfont/mkemoji write font.bfnt/emoji.bfnt into the current directory;
# mkemoji also reads font.bfnt (for the cell size), hence that dependency.
font.bfnt: mkfont $(TTF)
	./mkfont $(TTF)

emoji.bfnt: mkemoji font.bfnt $(wildcard emoji_src/*.png)
	./mkemoji

mkfont: mkfont.c charset.h stb_truetype.h
	$(HOSTCC) $(CFLAGS) -o $@ mkfont.c -lm

mkemoji: mkemoji.c emoji_charset.h stb_image.h
	$(HOSTCC) $(CFLAGS) -o $@ mkemoji.c -lm

$(STB):
	curl -fsSL -o $@ $(STB_URL)/$@

$(TTF):
	@echo "$@: not found. Install a monospace TTF (e.g. fonts-dejavu-core) or pass TTF=/path/to/font.ttf" >&2; false

install: all
	install -d $(DESTDIR)$(FONTDIR)
	install -m 644 font.bfnt emoji.bfnt $(DESTDIR)$(FONTDIR)

clean:
	rm -f mkfont mkemoji

distclean: clean
	rm -f $(STB) *.bfnt

# A failed or interrupted curl must not leave a truncated stb_*.h behind
# for the next run to treat as a valid cached copy.
.DELETE_ON_ERROR:
.PHONY: all install clean distclean
