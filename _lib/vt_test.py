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
print("## saved cursor after a screen shrink")
check("restore after shrink stays on screen", ["repro"], "inside the 16-row screen: yes")
print("## random escape-heavy traffic with resizes")
for seed in (1, 2, 3, 6, 7):
    check("seed %d, 100000 operations" % seed, [str(seed), "100000"], "no fault")
shutil.rmtree(tmp, ignore_errors=True)
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail))
sys.exit(1 if fail else 0)
