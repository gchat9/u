#!/usr/bin/env python3
# Tests _lib/heap.h (the allocator of vt.c and xterm_screen.c):  python3 _lib/heap_test.py
# Builds heap_test.c freestanding, as the tools are, for the host compiler and for each
# of the cross compilers found (i386 runs natively on an x86-64 kernel that allows it,
# the others under qemu-user), and runs it.  Size_t is 32 bits on i386 and armhf, which is
# what this is for.
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="heap-test-")
TARGETS = [("host", os.environ.get("CC", "gcc"), [], []),
           ("i386", "i686-linux-gnu-gcc", [], []),
           ("armhf", "arm-linux-gnueabihf-gcc", ["-mcpu=cortex-a15", "-mthumb"], ["qemu-arm"]),
           ("aarch64", "aarch64-linux-gnu-gcc", [], ["qemu-aarch64"]),
           ("riscv64", "riscv64-linux-gnu-gcc", [], ["qemu-riscv64"])]
ok = fail = 0
for name, cc, extra, runner in TARGETS:
    if not shutil.which(cc) or (runner and not shutil.which(runner[0])):
        print("  SKIP %s: no %s%s" % (name, cc, " or " + runner[0] if runner else "")); continue
    exe = os.path.join(tmp, "heap_test_" + name)
    b = subprocess.run([cc, "-Os", "-Wall", "-Wextra", "-static", "-nostdlib", "-ffreestanding",
                        "-fno-stack-protector", "-fno-pic", "-fno-pie", "-no-pie", "-fomit-frame-pointer",
                        "-Wno-attributes", *extra, "-Wl,-z,max-page-size=0x1000", "-Wl,--build-id=none",
                        "-Wl,-T," + os.path.join(HERE, "..", "_sys", "cleanup-re+rw.ld"),
                        os.path.join(HERE, "heap_test.c"), "-o", exe], capture_output=True, text=True)
    if b.returncode:
        fail += 1; print("  FAIL %s: does not build\n%s" % (name, b.stderr[-600:])); continue
    try:
        r = subprocess.run(runner + [exe], capture_output=True, text=True, timeout=120)
    except OSError as e:
        print("  SKIP %s: cannot run it (%s)" % (name, e)); continue
    good = r.returncode == 0 and "ok, problems 0" in r.stdout
    ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
    print("  %s %s%s" % ("PASS" if good else "FAIL", name, "" if good else "\n" + (r.stdout + r.stderr)[-600:]))
shutil.rmtree(tmp, ignore_errors=True)
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail))
sys.exit(1 if fail else 0)
