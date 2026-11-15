#!/usr/bin/env python3
# Tests xtty's BEL under an X server:  python3 xtty_demo/test_bell.py
# Builds bell.c (against ../_lib, as xtmux does) and checks the window's WM_HINTS:
# a BEL marks the window urgent only while it lacks focus, and gaining focus clears
# the mark.  Needs DISPLAY (an X server), xdotool, and xprop/xwininfo (x11-utils).
import os, subprocess, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..", "_lib")
exe = os.path.join(tempfile.mkdtemp(prefix="xtty-bell-"), "bell")
subprocess.run(["gcc", "-O2", "-D_GNU_SOURCE", "-DX11_BACKEND", "-DXTERM_LIB_ONLY", "-I" + LIB,
                "-I" + os.path.join(LIB, "..", "_font"), "-o", exe, os.path.join(HERE, "bell.c"),
                os.path.join(LIB, "xtty.c"), os.path.join(LIB, "xterm_screen.c"),
                os.path.join(LIB, "xterm.c"), os.path.join(LIB, "vt.c")], check=True)
def sh(*a): return subprocess.run(list(a), capture_output=True, text=True).stdout
def wm_hints(w): return sh("xprop", "-id", w, "WM_HINTS")
def urgent(w): return "urgency hint bit is set" in wm_hints(w)
def xd(*a): subprocess.run(["xdotool", *a])
root = sh("xwininfo", "-root").split("Window id:")[1].split()[0]
p = subprocess.Popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
ok = True
def check(name, cond, detail=""):
    global ok; ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + detail))
try:
    w = None
    for _ in range(50):
        for line in sh("xwininfo", "-root", "-tree").splitlines():
            if '"xtty bell test"' in line: w = line.split()[0]
        if w: break
        time.sleep(0.2)
    check("xtty window found", w is not None)
    if w is None: sys.exit(1)   # no window: nothing to test (the check above says so)
    time.sleep(1); xd("windowfocus", "--sync", w); time.sleep(0.5)
    check("no WM_HINTS before any bell", not urgent(w), wm_hints(w))
    xd("type", "--delay", "20", "b"); time.sleep(2.5)
    check("BEL while focused does not mark the window", not urgent(w), wm_hints(w))
    xd("type", "--delay", "20", "b"); xd("windowfocus", "--sync", root); time.sleep(2.5)
    check("BEL while unfocused marks the window urgent", urgent(w), wm_hints(w))
    xd("windowfocus", "--sync", w); time.sleep(0.8)
    check("gaining focus clears the urgency mark", not urgent(w), wm_hints(w))
finally:
    p.kill(); p.wait()
sys.exit(0 if ok else 1)
