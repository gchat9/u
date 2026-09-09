# Tiny implementations of some common Linux tools.

GPLv2. Linux only. Few select architectures: x86-64/aarch64/i686/armhf/riscv64

Main goal is to produce smallest possible executable for each supported platform. To that end, following deviations from coreutils, procps and friends are allowed:
- no -h (--help), no --version, no "usage" print at all. Users are supposed to know the tool or read relevant man pages;
- undefined (but harmless) behaviour on bogus input or arguments (see below);
- lack of support for some CLI options and modes of operation (stuff that *is* implemented is aimed to be compatible/comparable to established implementations).


## Undefined but harmless behaviour

Example: user runs "sleep" tool with some exceptionally long and/or corrupted argv[1].

- allowed behaviour: exit immideately, do what the tool normally does (sleep) for some undefined amount of time;

- not allowed behaviour: memory corruption.


## Repo organization

- `_arch/`   per-architecture bits (syscall macros, numbers, `_start`). C + assembly;
- `_sys/`    entry-point glue and shared type/constant definitions, common to all architectures. C;
- `_lib/`    small freestanding libc-ish helpers (mem, str, net);
- `[a-z].*/` individual tools. C only.

## LLM/AI usage

Yes.
