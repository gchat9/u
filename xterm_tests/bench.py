#!/usr/bin/env python3
# Usage: ./bench.py MODE [DEPTH] [BIN...]   (default BIN: ./xt_bench; N=runs via env, default 5)
# Prints the minimum CPU and wall time over N runs, under a private Xvfb.
import os, subprocess, sys, time, resource
mode = sys.argv[1]; depth = sys.argv[2] if len(sys.argv) > 2 else "24"
bins = sys.argv[3:] or ["./xt_bench"]; n = int(os.environ.get("N", "5"))
xv = subprocess.Popen(["Xvfb", ":2", "-screen", "0", "800x600x" + depth, "-nolisten", "tcp"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1); os.environ["DISPLAY"] = ":2"
for b in bins:
    cpu, wall = [], []
    for _ in range(n):
        r0 = resource.getrusage(resource.RUSAGE_CHILDREN); t0 = time.time()
        rc = subprocess.run([b, mode], stderr=subprocess.DEVNULL).returncode
        t1 = time.time(); r1 = resource.getrusage(resource.RUSAGE_CHILDREN); time.sleep(0.25)
        if rc == 0: cpu.append(r1.ru_utime + r1.ru_stime - r0.ru_utime - r0.ru_stime); wall.append(t1 - t0)
    print(f"{b} d{depth} mode {mode}: cpu {min(cpu):.3f}s  wall {min(wall):.3f}s")
xv.kill()
