# Tiny implementations of some common Linux tools.

GPLv2. Linux only. Few select CPUs: x86-64/aarch64/i686/armhf

Main goal is to produce smallest possible executable for each supported platform. To that end, following deviations from coreutils, procps and friends are allowed:
- no -h (--help), no --version, no "usage" print at all. Users are supposed to know the tool or read relevant man pages;
- undefined (but harmless) behaviour on bogus input or arguments (see below);
- lack of support for some CLI options and modes of operation (stuff that *is* implemented is aimed to be compatible/comparable to established implementations).


## Undefined but harmless behaviour

Example: user runs "sleep" tool with some exceptionally long and/or corrupted argv[1].

- allowed behaviour: exit immideately, do what the tool normally does (sleep) for some undefined amount of time;

- not allowed behaviour: memory corruption.


## Repo organization

- `_sys/`    shared arch-specific bits. C + assembly;
- `_.*/`     other shared bits (for future use, none exist at the moment);
- `[a-z].*/` individual tools. C only.

## LLM/AI usage

Yes.
