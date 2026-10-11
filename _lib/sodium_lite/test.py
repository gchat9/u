#!/usr/bin/env python3
# Tests sodium_lite against libsodium (the system's shared library, through
# ctypes) on random inputs, and against the published test vectors:
#     python3 _lib/sodium_lite/test.py
# Needs gcc and libsodium's runtime library (libsodium.so).
import ctypes, ctypes.util, hashlib, os, random, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="sodium-lite-test-")
so = os.path.join(tmp, "lite.so")
subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-shared", "-fPIC", "-o", so,
                os.path.join(HERE, "sodium_lite.c"), os.path.join(HERE, "randombytes.c")], check=True)
lite = ctypes.CDLL(so)
ref = ctypes.CDLL(ctypes.util.find_library("sodium") or "libsodium.so.23")
assert ref.sodium_init() >= 0
rnd = random.Random(20260710)
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1
    else: fail += 1
    print("  %s %s %s" % ("PASS" if cond else "FAIL", name, "" if cond else info))
def rb(n): return bytes(rnd.getrandbits(8) for _ in range(n))
def B(b): return ctypes.create_string_buffer(bytes(b), len(b))
def out(n): return ctypes.create_string_buffer(n)
def chunks(b):                                  # random split points
    i, parts = 0, []
    while i < len(b):
        n = rnd.choice([0, 1, 2, 15, 16, 17, 63, 64, 65, 127, 128, 129, rnd.randrange(0, 300)])
        parts.append(b[i:i + n]); i += n
    return parts
U = ctypes.c_ulonglong

print("## SHA-256")
def sha(lib, parts):
    st = out(256); o = out(32)
    lib.crypto_hash_sha256_init(st)
    for p in parts: lib.crypto_hash_sha256_update(st, p, U(len(p)))
    lib.crypto_hash_sha256_final(st, o)
    return o.raw
check("abc (FIPS 180 vector)", sha(lite, [b"abc"]).hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
bad = [n for n in list(range(0, 260)) + [1000, 4097, 100000] if sha(lite, [bytes(range(256)) * 400][0:1] and [(bytes(range(256)) * 400)[:n]]) != hashlib.sha256((bytes(range(256)) * 400)[:n]).digest()]
check("lengths 0..259 and larger, one-shot path of update, against hashlib", not bad, bad[:5])
bad = 0
for _ in range(300):
    m = rb(rnd.randrange(0, 700)); parts = chunks(m)
    if sha(lite, parts) != hashlib.sha256(m).digest(): bad += 1
check("random messages in random pieces (300)", bad == 0, bad)
m = rb(777); o = out(32); lite.crypto_hash_sha256(o, m, U(len(m)))
check("one-shot crypto_hash_sha256", o.raw == hashlib.sha256(m).digest())

print("## ChaCha20")
def chacha(lib, m, n, ic, k, inplace=False):
    buf = B(m); o = buf if inplace else out(len(m))
    lib.crypto_stream_chacha20_xor_ic(o, buf, U(len(m)), n, ctypes.c_uint64(ic), k)
    return o.raw[:len(m)]
bad = 0
for i in range(600):
    k, n = rb(32), rb(8)
    ic = rnd.choice([0, 1, 2, 0xffffffff, 0xfffffffe, 0xffffffffffffffff, rnd.getrandbits(64)])
    m = rb(rnd.choice([0, 1, 63, 64, 65, 127, 128, 129, rnd.randrange(0, 1500)]))
    a = chacha(lite, m, n, ic, k); b = chacha(ref, m, n, ic, k)
    if a != b or chacha(lite, m, n, ic, k, True) != b: bad += 1
check("random key, nonce, counter (incl. 32/64-bit wraps), length; in place too (600)", bad == 0, bad)
check("RFC 7539 appendix A.1 keystream block 0 (zero key and nonce)",
      chacha(lite, bytes(64), bytes(8), 0, bytes(32)).hex() == "76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586")

print("## Poly1305")
def poly(lib, key, parts):
    st = out(512); o = out(16)
    lib.crypto_onetimeauth_poly1305_init(st, key)
    for p in parts: lib.crypto_onetimeauth_poly1305_update(st, p, U(len(p)))
    lib.crypto_onetimeauth_poly1305_final(st, o)
    return o.raw
key = bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")
msg = b"Cryptographic Forum Research Group"
check("RFC 7539 2.5.2 vector", poly(lite, key, [msg]).hex() == "a8061dc1305136c6c22b8baf0c0127a9")
bad = 0
for i in range(800):
    k = rb(32) if i % 4 else (b"\xff" * 32 if i % 8 == 0 else bytes(32))
    m = rb(rnd.randrange(0, 400)) if i % 5 else b"\xff" * rnd.randrange(0, 400)
    if poly(lite, k, chunks(m)) != poly(ref, k, [m]): bad += 1
check("random keys (incl. extremes), messages (incl. all 0xff), random pieces (800)", bad == 0, bad)

print("## X25519")
def x25519(lib, n, p):
    q = out(32); r = lib.crypto_scalarmult_curve25519(q, n, p); return r, q.raw
def x25519b(lib, n):
    q = out(32); r = lib.crypto_scalarmult_curve25519_base(q, n); return r, q.raw
alice = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
check("RFC 7748 6.1: Alice's public key", x25519b(lite, alice)[1].hex() == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
bobpk = bytes.fromhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
check("RFC 7748 6.1: the shared secret", x25519(lite, alice, bobpk)[1].hex() == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
bad = 0
for i in range(300):
    n, p = rb(32), rb(32)
    if x25519(lite, n, p) != x25519(ref, n, p) or x25519b(lite, n) != x25519b(ref, n): bad += 1
check("random scalars and points, results and return codes (300)", bad == 0, bad)
bad = []
for p in [bytes(32), b"\x01" + bytes(31), b"\xff" * 32, bytes.fromhex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
          bytes.fromhex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157")]:
    n = rb(32)
    if x25519(lite, n, p) != x25519(ref, n, p): bad.append(p.hex()[:8])
check("small-order and non-canonical points: -1 exactly when libsodium says -1", not bad, bad)
pk, sk = out(32), out(32); lite.crypto_box_keypair(pk, sk)
check("crypto_box_keypair: pk is the base multiple of sk", x25519b(ref, sk.raw)[1] == pk.raw and sk.raw != bytes(32))

print("## Ed25519")
def keypair(seed):
    pk, sk = out(32), out(64); ref.crypto_sign_ed25519_seed_keypair(pk, sk, seed); return pk.raw, sk.raw
def sign(lib, m, sk):
    s = out(64); n = U(0); lib.crypto_sign_ed25519_detached(s, ctypes.byref(n), m, U(len(m)), sk); return s.raw, n.value
def verify(lib, s, m, pk): return lib.crypto_sign_verify_detached(s, m, U(len(m)), pk)
seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
pk, sk = keypair(seed)
want = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
check("RFC 8032 7.1 test 1: signature of the empty message", sign(lite, b"", sk)[0].hex() == want)
check("...and it verifies", verify(lite, bytes.fromhex(want), b"", pk) == 0)
bad = bad2 = bad3 = 0
for i in range(120):
    pk, sk = keypair(rb(32)); m = rb(rnd.choice([0, 1, 31, 32, 33, 64, 111, 112, 113, 127, 128, 129, rnd.randrange(0, 800)]))
    s, n = sign(lite, m, sk)
    if s != sign(ref, m, sk)[0] or n != 64: bad += 1
    if verify(lite, s, m, pk) != 0: bad2 += 1
    t = bytearray(s); t[rnd.randrange(64)] ^= 1 << rnd.randrange(8)
    m2 = bytearray(m + b"x"); m2[rnd.randrange(len(m2))] ^= 1 << rnd.randrange(8)
    p2 = bytearray(pk); p2[rnd.randrange(32)] ^= 1 << rnd.randrange(8)
    for sg, mm, pp in ((bytes(t), m, pk), (s, bytes(m2), pk), (s, m, bytes(p2))):
        if verify(lite, sg, mm, pp) != verify(ref, sg, mm, pp): bad3 += 1
check("signatures equal libsodium's, random seeds and messages (120)", bad == 0, bad)
check("libsodium's signatures verify (120)", bad2 == 0, bad2)
check("damaged signature, message or key: same verdict as libsodium (360)", bad3 == 0, bad3)
L = (1 << 252) + 27742317777372353535851937790883648493
pk, sk = keypair(rb(32)); m = b"malleable?"; s, _ = sign(ref, m, sk)
s2 = s[:32] + ((int.from_bytes(s[32:], "little") + L).to_bytes(32, "little"))
check("S + L (non-canonical) is rejected, as libsodium does", verify(lite, s2, m, pk) == -1 and verify(ref, s2, m, pk) == -1)
check("a public key that is no curve point is rejected", verify(lite, s, m, b"\xff" * 31 + b"\x7f") == verify(ref, s, m, b"\xff" * 31 + b"\x7f") == -1)
ident = b"\x01" + b"\0" * 31                   # the identity point: a public key of small order
BASE = bytes.fromhex("5866666666666666666666666666666666666666666666666666666666666666")
forged = BASE + (1).to_bytes(32, "little")         # [S]B with R = B: "verifies" for any message if the key is ident
check("a small-order public key cannot forge signatures (the identity)",
      verify(lite, forged, b"any message", ident) == verify(ref, forged, b"any message", ident) == -1)
check("a small-order R is rejected, as libsodium does",
      verify(lite, ident + bytes(32), b"m", pk) == verify(ref, ident + bytes(32), b"m", pk) == -1)

print("## misc")
check("sodium_memcmp: 0 if equal, -1 if not, over lengths and positions",
      all(lite.sodium_memcmp(B(a), B(b), len(a)) == (0 if a == b else -1)
          for a, b in [(b"", b""), (b"a", b"a"), (b"a", b"b"), (b"abcd" * 9, b"abcd" * 9), (b"abcd" * 9, b"abcd" * 8 + b"abce"), (b"\x00" * 33, b"\x80" + b"\x00" * 32)]))
z = B(b"secret" * 10); lite.sodium_memzero(z, 60)
check("sodium_memzero", z.raw[:60] == bytes(60))
a, b = out(64), out(64); lite.randombytes_buf(a, 64); lite.randombytes_buf(b, 64)
big = out(100000); lite.randombytes_buf(big, 100000)
check("randombytes_buf: differs between calls, fills big buffers, not biased",
      a.raw != b.raw and a.raw != bytes(64) and abs(sum(big.raw) / 100000 - 127.5) < 2 and len(set(big.raw)) == 256)
check("sodium_init", lite.sodium_init() == 0)

print("## speed (this machine)")
def rate(f, n, unit):
    t = time.time(); f(); d = time.time() - t; return "%.1f %s" % (n / d, unit)
m = B(rb(1 << 20)); o = out(1 << 20); k, nn = B(rb(32)), B(rb(8))
mb = lambda lib, f: rate(lambda: [f(lib) for _ in range(8)], 8, "MB/s")
print("  ChaCha20   lite", mb(lite, lambda l: l.crypto_stream_chacha20_xor_ic(o, m, U(1 << 20), nn, ctypes.c_uint64(0), k)),
      " libsodium(ref impl)", mb(ref, lambda l: l.crypto_stream_chacha20_xor_ic(o, m, U(1 << 20), nn, ctypes.c_uint64(0), k)))
def polyrun(l):
    st = out(512); mac = out(16); l.crypto_onetimeauth_poly1305_init(st, k); l.crypto_onetimeauth_poly1305_update(st, m, U(1 << 20)); l.crypto_onetimeauth_poly1305_final(st, mac)
print("  Poly1305   lite", mb(lite, polyrun), " libsodium", mb(ref, polyrun))
pk, sk = keypair(rb(32)); msg = rb(200)
def ops(lib, f, n=30): return "%.2f ms" % (1000 * (lambda t: (time.time() - t) / n)((lambda t: ([f(lib) for _ in range(n)], t)[1])(time.time())))
print("  Ed25519 sign   lite", ops(lite, lambda l: sign(l, msg, sk)), " libsodium", ops(ref, lambda l: sign(l, msg, sk)))
sg = sign(ref, msg, sk)[0]
print("  Ed25519 verify lite", ops(lite, lambda l: verify(l, sg, msg, pk)), " libsodium", ops(ref, lambda l: verify(l, sg, msg, pk)))
print("  X25519         lite", ops(lite, lambda l: x25519(l, rb(32), rb(32))), " libsodium", ops(ref, lambda l: x25519(l, rb(32), rb(32))))

print("\nSUMMARY: %d/%d passed" % (ok, ok + fail))
sys.exit(1 if fail else 0)
