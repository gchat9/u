#!/usr/bin/env python3
# Tests the terminal emulator's parser (vt.c, used by xmssh) against the traffic a
# hostile server can send, and against screen resizes:
#     python3 _lib/vt_test.py
# Builds vt_fuzz.c with vt.c, under AddressSanitizer and UBSan when the toolchain
# has them (otherwise plainly), and runs a deterministic reproducer and random runs.
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="vt-test-")
exe = os.path.join(tmp, "vt_fuzz")
base = ["gcc", "-g", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I" + HERE, "-I" + os.path.join(HERE, "..", "_font"),
        os.path.join(HERE, "vt_fuzz.c"), os.path.join(HERE, "vt.c"), "-o", exe]
san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
if subprocess.run(base[:-2] + san + ["-o", exe]).returncode == 0:
    print("built with AddressSanitizer and UBSan")
else:
    print("building without sanitizers")
    subprocess.run(base, check=True)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
ok = fail = 0
def check(name, args, want_out):
    global ok, fail
    p = subprocess.run([exe] + args, capture_output=True, text=True, env=env, timeout=600)
    good = p.returncode == 0 and want_out in p.stdout and "runtime error" not in p.stderr \
        and "AddressSanitizer" not in p.stderr
    ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
    print("  %s %s" % ("PASS" if good else "FAIL", name) + ("" if good else "\n" + (p.stdout + p.stderr)[-800:]))
print("## DEC special graphics (line drawing) and its reset")
check("ESC ( 0 maps lines, ESC ( B and RIS restore ASCII", ["dec"], "dec: 9484 9472 9472 9488 120 113 ris: 113")
print("## BEL: a bell, but not the end of an OSC or DCS string")
check("BEL sets the bell flag; OSC (BEL or ST) and DCS ends do not", ["bell"], "bell: 1 0 0 0")
print("## alternate screen: restored on leaving, grid given back")
check("primary comes back, alt starts blank, grid freed", ["alt"], "alt: primary PQ blank 1 freed 2 in_alt 0")
print("## small cells against full-size cells (same traffic, same screen)")
for seed, mode in ((1, 0), (2, 0), (3, 1), (4, 1), (5, 1), (6, 2), (7, 2), (8, 3), (9, 3), (10, 3)):
    check("seed %d, mode %d, 20000 steps" % (seed, mode), ["diff", str(seed), "20000", str(mode)], "identical")
print("## style table: full, then cleaned up")
check("127 styles and a 3-line scroll widen the grid safely; styles that come and go do not",
      ["styles"], "styles: full table widened 1, many styles in turn stayed small 1")
print("## invalid UTF-8: nothing drawn for it, nothing lost after it")
check("overlong, surrogate and too-large forms are dropped; a cut-short sequence does not eat the next byte",
      ["utf8"], "[ab|][ab|][ab|][ab|][x|][x<e9>|][|xy][z|][<e9><2500>|]")
print("## saved cursor after a screen shrink")
check("restore after shrink stays on screen", ["repro"], "inside the 16-row screen: yes")
print("## random escape-heavy traffic with resizes")
for seed in (1, 2, 3, 6, 7):
    check("seed %d, 100000 operations" % seed, [str(seed), "100000"], "no fault")
shutil.rmtree(tmp, ignore_errors=True)
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail))
sys.exit(1 if fail else 0)
