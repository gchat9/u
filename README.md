# Tiny implementations of some common Linux tools.

GPLv2. Linux only. Few select architectures: x86-64/aarch64/i686/armhf/riscv64

Main goal is to produce smallest possible executable for each supported platform. To that end, following deviations from coreutils, procps and friends are allowed:
- no -h (--help), no --version, no "usage" print at all. Users are supposed to know the tool or read relevant man pages;
- undefined (but harmless) behaviour on bogus input or arguments (see below);
- lack of support for some CLI options and modes of operation (stuff that *is* implemented is aimed to be compatible/comparable to established implementations).


## Undefined but harmless behaviour

Example: user runs "sleep" tool with some exceptionally long and/or corrupted argv[1], or "cat" on a file that vanishes mid-read.

- allowed behaviour: exit immideately, do what the tool normally does (sleep) for some undefined amount of time;

- not allowed behaviour: memory corruption.


## Repo organization

- `_arch/`   per-architecture bits (syscall macros, numbers, `_start`). C + assembly;
- `_sys/`    entry-point glue and shared type/constant definitions, common to all architectures. C;
- `_lib/`    small freestanding libc-ish helpers (mem, str, net), plus `xterm`, an X11 terminal surface (opt-in: not pulled in by `_sys/_.h`, built by the tools that use it), `vt`, the VT parser it can display, `xterm_screen`, which shows a parsed screen in the window and handles the mouse, selection and clipboard for it, `xtty`, all of that behind a write/poll interface for a program that wants to be an X11 terminal app (see `xtty_demo/`), `xterm_conf.h`, their compile-time options, and `sodium_lite/`, a small portable subset of libsodium (SHA-256, ChaCha20, Poly1305, X25519, Ed25519) tested against the real one;
- `_mk/`     shared Makefile logic, included from each tool's own Makefile;
- `_font/`   generator for the font files `xterm` loads at runtime: `mkfont`/`mkemoji`, their inputs (`emoji_src/`), and `charset.h`/`emoji_charset.h`, which define what each glyph is. Host tools, not a tool itself;
- `[a-z].*/` individual tools. C only. `mssh/` is a minimal SSH-2 client and `xmssh/` the same client in a window of its own, with a built-in xterm emulator (`_lib/xtty`) instead of the user's terminal (it loads `font.bfnt` at run time: `make fonts`, then install). They share `mssh/mssh.c`, the protocol; only `tty.c`, the terminal front-end, differs (see `mssh/mssh.h`). Their cryptography is `_lib/sodium_lite`, the few libsodium functions they use (`make SODIUM=system` links the real libsodium instead, to compare). Like `tmux/` and `xtmux/`, dietlibc programs, not freestanding.

## Building

Each tool builds standalone (`make -C pwd`, or `cd sleep && make`).

From the repo root:
- `make` builds every tool in place, using the host compiler;
- `make x86_64` / `i386` / `aarch64` / `armhf` / `riscv64` cross-builds every tool for that architecture, `sstrip`s the result, and collects it under `_release/<arch>/`; `make release-all` does all five;
- `make release ARCH=<name> [CC=<compiler>]` does the same for an architecture not in that built-in list;
- `make clean` / `make distclean` clean per-tool build output, `distclean` also removes `_release/`;
- `make fonts` builds `font.bfnt` and `emoji.bfnt` in `_font/` (separate from `make` because it needs a monospace TTF, a TTF/TTC with the double-width glyphs (Noto Sans CJK by default) and, on first use, network access for the pinned `stb_*.h`); `make -C _font install` copies them to `/etc`, where `xterm` looks. `distclean` removes the downloaded headers and generated fonts.

## LLM/AI usage

Yes.
