#!/usr/bin/env python3
# Tests xtty (and so the layers under it) through xtty_demo, on Xvfb:  python3 xtty_demo/test.py
# Needs what xtmux/tests do: Xvfb, xdotool, xclip, and its mouse_lib helpers.
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "xtmux", "tests", "mouse"))
from mouse_lib import *

DEMO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xtty_demo")
LOG = "/tmp/xtty_demo.log"
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1
    else: fail += 1
    print("  %s %s %s" % ("PASS" if cond else "FAIL", name, "" if cond else info))
def xc(sel): return subprocess.run(["xclip", "-o", "-selection", sel], capture_output=True, timeout=5).stdout
def own(sel, data): subprocess.run(["xclip", "-i", "-selection", sel], input=data, timeout=5); time.sleep(.3)
def log(): return open(LOG).read() if os.path.exists(LOG) else ""
def hexlog(): return log().replace("\n", "")
def run(*args):
    if os.path.exists(LOG): os.unlink(LOG)
    p = subprocess.Popen([DEMO, "-l", LOG, *args])
    for _ in range(100):                    # until the window is up and the program has had its first reply
        if "1b5b343b3152" in log(): break
        time.sleep(.1)
    time.sleep(.3); return p
def drag(c0, r0, c1, r1):
    move(c0, r0); xdo("mousedown", "1"); time.sleep(.1); move(c1, r1); time.sleep(.1); xdo("mouseup", "1"); time.sleep(.3)
def mclick(c, r, n):
    move(c, r)
    for _ in range(n): xdo("click", "1"); time.sleep(.05)
    time.sleep(.3)

xv = start_xvfb()
try:
    print("## a program with no terminal behind it")
    p = run()
    check("window comes up and the program gets its cursor position report", "1b5b343b3152" in log(), log())
    xdo("type", "ab"); time.sleep(.5)
    check("typed keys arrive as input", hexlog().endswith("6162"), log())
    own("primary", b"hello"); xdo("key", "ctrl+v"); time.sleep(.5)
    check("Ctrl+V pastes, bracketed as the program asked", hexlog().endswith("1b5b3230307e68656c6c6f1b5b3230317e"), log())
    own("primary", b"sentinel"); drag(0, 1, 10, 1)
    check("drag selects and copies", xc("primary") == b"foo bar-baz", xc("primary"))
    mclick(6, 1, 2); check("double click selects a word", xc("primary") == b"bar-baz", xc("primary"))
    mclick(3, 2, 3); check("triple click selects a line", xc("primary") == b"second line here", xc("primary"))
    wid = subprocess.run(["xdotool", "search", "--name", "xtty demo"], capture_output=True, text=True).stdout.split()[0]
    xdo("windowsize", wid, "520", "260"); time.sleep(.8)
    check("resizing is reported (520x260 px = 40x10 cells), the program redraws", "size 10x40" in log(), log())
    p.kill(); p.wait()

    print("## a program that asks for the mouse")
    p = run("-m")
    click(5, 2); time.sleep(.4)
    check("click is reported, SGR", "1b5b3c303b363b334d" in hexlog() and "1b5b3c303b363b336d" in hexlog(), log())
    own("primary", b"sentinel2"); xdo("keydown", "shift"); drag(0, 1, 10, 1); xdo("keyup", "shift")
    check("Shift keeps the mouse for copying", xc("primary") == b"foo bar-baz", xc("primary"))
    p.kill(); p.wait()
finally:
    xv.kill()
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail)); sys.exit(1 if fail else 0)
