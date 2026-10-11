#!/usr/bin/env python3
# The constant tables of sodium_lite.c, computed from their definitions (the
# result is pasted into the C file): the SHA-2 round constants and initial
# values, and the Curve25519 / Ed25519 field and group constants as the
# 16 x 16-bit limbs (little-endian) its field arithmetic uses.
import math

def primes(n):
    out, c = [], 2
    while len(out) < n:
        if all(c % p for p in out if p * p <= c): out.append(c)
        c += 1
    return out

def icbrt(n):                      # floor(cube root of n)
    lo, hi = 0, 1 << (n.bit_length() // 3 + 2)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if mid ** 3 <= n: lo = mid
        else: hi = mid - 1
    return lo

def frac_root(p, bits, root):      # first `bits` bits of the fractional part of p**(1/root)
    if root == 2: v = math.isqrt(p << (2 * bits))
    else:         v = icbrt(p << (3 * bits))
    return v & ((1 << bits) - 1)

def c_array(name, vals, bits, per):
    w = bits // 4
    rows = [", ".join("0x%0*x" % (w, v) for v in vals[i:i + per]) for i in range(0, len(vals), per)]
    return "static const uint%d_t %s[%d] = {\n    %s,\n};" % (bits, name, len(vals), ",\n    ".join(rows))

P = (1 << 255) - 19
def inv(x): return pow(x, P - 2, P)
def limbs(x): return [(x >> (16 * i)) & 0xffff for i in range(16)]
d  = (-121665 * inv(121666)) % P
by = (4 * inv(5)) % P
bx2 = ((by * by - 1) * inv(d * by * by + 1)) % P
bx = pow(bx2, (P + 3) // 8, P)
if (bx * bx - bx2) % P: bx = bx * pow(2, (P - 1) // 4, P) % P
if bx & 1: bx = P - bx             # the base point has an even x
L = (1 << 252) + 27742317777372353535851937790883648493

if __name__ == "__main__":
    print(c_array("K256", [frac_root(p, 32, 3) for p in primes(64)], 32, 8))
    print(c_array("K512", [frac_root(p, 64, 3) for p in primes(80)], 64, 4))
    print("/* H256 */", ", ".join("0x%08x" % frac_root(p, 32, 2) for p in primes(8)))
    print("/* H512 */", ", ".join("0x%016x" % frac_root(p, 64, 2) for p in primes(8)))
    for n, v in (("D", d), ("D2", 2 * d % P), ("X", bx), ("Y", by), ("I", pow(2, (P - 1) // 4, P))):
        print("static const gf %s = {%s};" % (n, ", ".join("0x%04x" % x for x in limbs(v))))
    print("/* L */", ", ".join("0x%02x" % ((L >> (8 * i)) & 255) for i in range(32)))
