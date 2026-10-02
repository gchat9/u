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
