#!/usr/bin/env python3
# Prints the emoji_wide_ranges[] table for emoji_charset.h.
#
# Rule: every code point in every emitted range is either East Asian Width
# "W" (double-width, which is what wcwidth() -- and therefore readline --
# reports for emoji) or currently unassigned. Runs are merged only across
# unassigned gaps, so emoji added to these blocks in later Unicode versions
# are covered without regenerating, and no narrow character is ever swept in.
# Astral entries are stored as (cp - 0x10000) so everything fits in uint16_t.
import unicodedata as u
def W(c): return u.east_asian_width(chr(c)) == 'W'
def free(c): return u.category(chr(c)) == 'Cn'
def runs(lo, hi):
    out, c = [], lo
    while c <= hi:
        if not W(c): c += 1; continue
        s = e = c
        while True:
            n = e + 1
            if n <= hi and W(n): e = n; continue
            g = n
            while g <= hi and free(g): g += 1
            if g > n and g <= hi and W(g): e = g; continue
            break
        out.append((s, e)); c = e + 1
    return out
bmp = [r for r in runs(0x2300, 0x2BFF) if r != (0x2329, 0x232A)]  # angle brackets: CJK punctuation, not emoji
astral = runs(0x1F000, 0x1FAFF)
print("/* Unicode %s */" % u.unidata_version)
row = []
for s, e in bmp + [(s - 0x10000, e - 0x10000) for s, e in astral]:
    row.append("{0x%04X,0x%04X}" % (s, e))
    if len(row) == 6: print("    " + ", ".join(row) + ","); row = []
if row: print("    " + ", ".join(row) + ",")

# ---- the second table, for charset.h's charset_cjk_ranges[] ----
# What wcwidth() reports as double-width outside the emoji blocks above and
# outside the fullwidth forms U+FF01..FF60 (charset_fullwidth() handles
# those itself). libc takes the CJK blocks whole -- unassigned and East
# Asian "Ambiguous" code points included -- except combining marks, which
# are width 0; so: those blocks minus the marks, plus the astral runs that
# are East Asian Wide. Run: ./gen_wide_ranges.py cjk (prints the table
# above first, then this one)
import sys
if len(sys.argv) > 1 and sys.argv[1] == "cjk":
    def mark(c): return u.category(chr(c)) in ("Mn", "Me")
    def blocks(lo, hi):                  # [lo,hi] minus combining marks
        out, s = [], None
        for c in range(lo, hi + 2):
            if c <= hi and not mark(c):
                if s is None: s = c
            elif s is not None: out.append((s, c - 1)); s = None
        return out
    cjk = []
    for lo, hi in [(0x1100, 0x115F), (0x2329, 0x232A), (0x2E80, 0x303E), (0x3041, 0xA4CF),
                   (0xA960, 0xA97F), (0xAC00, 0xD7A3), (0xF900, 0xFAFF), (0xFE10, 0xFE19),
                   (0xFE30, 0xFE6F), (0xFFE0, 0xFFE6)]:
        cjk += blocks(lo, hi)
    def W(c): return u.east_asian_width(chr(c)) in ("W", "F")
    cjk += runs(0x16FE0, 0x1B2FF) + runs(0x20000, 0x3FFFD)
    print("/* Unicode %s */" % u.unidata_version)
    print(", ".join("{0x%04X,0x%04X}" % r for r in cjk))
