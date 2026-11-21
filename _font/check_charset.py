#!/usr/bin/env python3
# Checks charset.h against the keyboard tables of _lib/xterm.c: for each of the
# options XTERM_GREEK, XTERM_LATIN2 and XTERM_CYRILLIC, every character that a
# key can type with it (the keysym tables of xterm.c) is a glyph of the font made
# with it, and nothing is there twice; and that the font without options is what it
# was (the slots of the base set do not move).  Finally it checks the options as
# _lib/xterm_conf.h has them, which are the ones the fonts and the programs get.
# Run as:  python3 _font/check_charset.py   (make -C _font check)
import os, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, "..", "_lib", "xterm.c")).read()

def arr(name):
    m = re.search(r"static const uint16_t %s\[\] = \{(.*?)\};" % name, src, re.S)
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", re.sub(r"/\*.*?\*/", "", m.group(1)))]
lat2, cyr = arr("lat2_tab"), arr("cyr_tab")
def latin2(ks):
    if ks == 0x1ff: return 0x2d9
    if ks < 0x1e0: return lat2[ks - 0x1a1]
    cp = lat2[ks - 0x1c1]; return cp + 1 if cp else 0
def cyrillic(ks): return cyr[ks - 0x6a1] if ks < 0x6e0 else cyr[ks - 0x6c1] - 0x20
def greek(ks):
    if 0x7c1 <= ks <= 0x7d8: return 0x391 + (ks - 0x7c1) + (ks >= 0x7d2)
    if 0x7e1 <= ks <= 0x7f9: return (0x3b1 + (ks - 0x7e1)) ^ (ks in (0x7f2, 0x7f3))
    return 0
# the characters of cp_pairs under XTERM_LATIN2, and the comma-below letters that the xkb files
# give as Unicode keysyms (ro)
pairs = re.search(r"#if XTERM_LATIN2\n((?:\s+0x[0-9a-f]+, 0x[0-9a-f]+,.*\n)+)#endif", src).group(1)
turkish = {int(b, 16) for a, b in re.findall(r"(0x[0-9a-f]+), (0x[0-9a-f]+),", pairs)}
typed = {
    "XTERM_GREEK":    {greek(k) for k in range(0x7c1, 0x800)},
    "XTERM_LATIN2":   {latin2(k) for k in range(0x1a1, 0x200) if k - 0x1a1 < 63 or k >= 0x1c1 or k == 0x1ff} | turkish
                      | {0x218, 0x219, 0x21a, 0x21b} | {0xC1, 0xCD, 0xDA, 0xDD, 0xED, 0xFA, 0xFD},
    "XTERM_CYRILLIC": {cyrillic(k) for k in range(0x6a1, 0x700)},
}
for v in typed.values(): v.discard(0)

def glyphs(opts):
    """The codepoints of the narrow glyphs of charset.h with exactly the options opts on
    (None: with the options as _lib/xterm_conf.h has them, which is what the fonts get).
    xterm_conf.h has no -D overrides, so a copy of the tree's two headers is edited."""
    c = r'''#include "charset.h"
#include <stdio.h>
int main(void){ for (int i = 0; i < CHARSET_COUNT; i++) printf("%x\n", charset_codepoint(i)); return 0; }'''
    with tempfile.TemporaryDirectory() as d:
        os.mkdir(d + "/_font"); os.mkdir(d + "/_lib")
        shutil.copy(HERE + "/charset.h", d + "/_font")
        conf = open(HERE + "/../_lib/xterm_conf.h").read()
        if opts is not None:
            for o in typed:
                conf, n = re.subn(r"(?m)^#define %s \d+" % o, "#define %s %d" % (o, o in opts), conf)
                assert n == 1, o
        open(d + "/_lib/xterm_conf.h", "w").write(conf)
        open(d + "/_font/c.c", "w").write(c)
        subprocess.run(["cc", "-o", d + "/c", d + "/_font/c.c"], check=True)
        return [int(x, 16) for x in subprocess.run([d + "/c"], capture_output=True, text=True, check=True).stdout.split()]

def configured():
    """The language options that xterm_conf.h turns on."""
    conf = open(HERE + "/../_lib/xterm_conf.h").read()
    return [o for o in typed if re.search(r"(?m)^#define %s [1-9]" % o, conf)]

bad = 0
base = glyphs([])
conf_opts = configured()
for opts in ([], *[[o] for o in typed], list(typed), None):
    g = glyphs(opts)
    if opts is None: opts = conf_opts    # the file as it is: what the fonts and the programs get
    name = "+".join(o[6:] for o in opts) or "base"
    if opts is conf_opts: name = "xterm_conf.h: " + (name if opts else "none")
    problems = []
    if len(set(g)) != len(g): problems.append("duplicate codepoints")
    if len(g) > 511: problems.append("%d glyphs: more than 511" % len(g))
    if g[:len(base)] != base: problems.append("the base slots moved")
    for o in opts:
        missing = sorted(typed[o] - set(g))
        if missing: problems.append("%s: no glyph for %s" % (o, " ".join("U+%04X" % c for c in missing)))
        extra = sorted(set(g[len(base):]) - set().union(*[typed[x] for x in opts]))
        if extra: problems.append("not typeable, in the font only for %s: %s" % (o, " ".join("U+%04X" % c for c in extra)))
    print("%-24s %3d narrow glyphs  %s" % (name, len(g), "ok" if not problems else "FAIL"))
    for p in problems: print("    " + p)
    bad += bool(problems)
sys.exit(1 if bad else 0)
