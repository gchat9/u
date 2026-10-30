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
    # FIXME: a start sometimes fails with "xread failed" (the X server closes the connection
    # during setup), about once in a few suite runs, in whichever test starts a demo right
    # after another one was killed. The cause is not known. A pause before each start is
    # kept as a stop-gap, but it does not measurably prevent the failure (see NOTES in
    # the review); a retry of the setup, which keeps a real failure visible, is the
    # candidate fix, and it is not done yet.
    time.sleep(0.1)
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

# A 1000x780 screen: 780/26 = 30 lines, so the start size is the base size, 2 (13x26
# cells), which the size tests below count from.
os.environ["XRES"] = "1000x780x24"
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

    # Cells per size (font<k>, the header): 0 9x18, 1 11x22, 2 13x26, 3 15x31, 4 18x37,
    # 5 22x44, 6 26x52, 7 31x62.  At 520x260 px that is 14x57, 11x47, 10x40, 8x34, 7x28,
    # 5x23, 5x20, 4x16 (rows x columns).
    print("## font size")
    p = run()
    wid = subprocess.run(["xdotool", "search", "--name", "xtty demo"], capture_output=True, text=True).stdout.split()[0]
    def winsize():
        out = subprocess.run(["xdotool", "getwindowgeometry", wid], capture_output=True, text=True).stdout
        return out.split("Geometry: ")[1].split()[0]
    def inputs(): return [l for l in log().splitlines() if l and not l.startswith("size")]
    def last(): return log().splitlines()[-1]
    def count(): return log().count("size ")
    xdo("windowsize", wid, "520", "260"); time.sleep(.8)
    check("size 2, the base, is the start here: 520x260 px = 10x40 cells", "size 10x40" in log(), log())
    keys_in = inputs()
    xdo("key", "ctrl+equal"); time.sleep(.6)
    check("Ctrl+= one size larger (size 3): 8x34 cells", last() == "size 8x34", log())
    check("...the window keeps its size", winsize() == "520x260", winsize())
    for _ in range(3): xdo("key", "ctrl+equal"); time.sleep(.4)
    check("three more: size 6, twice the base: 5x20 cells", last() == "size 5x20", log())
    xdo("key", "ctrl+equal"); time.sleep(.6)
    check("size 7, the largest: 4x16 cells", last() == "size 4x16", log())
    n = count()
    xdo("key", "ctrl+equal"); time.sleep(.6)
    check("Ctrl+= at the largest size changes nothing", count() == n, log())
    xdo("key", "ctrl+KP_Subtract"); time.sleep(.6)
    check("Ctrl+KP_Subtract (keypad) one size smaller: 5x20", last() == "size 5x20", log())
    xdo("key", "ctrl+KP_Add"); time.sleep(.6)
    check("Ctrl+KP_Add (keypad) one size larger: 4x16", last() == "size 4x16", log())
    for _ in range(7): xdo("key", "ctrl+minus"); time.sleep(.4)
    check("Ctrl+- seven times: size 0, the smallest: 14x57 cells", last() == "size 14x57", log())
    n = count()
    xdo("key", "ctrl+minus"); time.sleep(.6)
    check("Ctrl+- at the smallest size changes nothing", count() == n, log())
    check("the zoom keys are not typed to the program", inputs() == keys_in, inputs())
    n = count()
    xdo("key", "ctrl+underscore"); time.sleep(.6)
    check("Ctrl+_ goes to the program (0x1f, readline and Emacs undo), the size stays",
          inputs()[-1] == "1f" and count() == n, inputs()[-3:])
    xdo("type", "x"); time.sleep(.5)
    check("ordinary keys still are", inputs()[-1] == "78", inputs())
    p.kill(); p.wait()

    # A 4K screen: the window is the whole screen.  Size 1 (11x22 cells) would need
    # 98 x 349 = 34202 cells, more than the grid buffer holds (32768), so it is refused
    # and the size stays at 2, with no rows lost.
    print("## font size on a 4K screen: a smaller size the grid cannot hold is refused")
    xv.kill(); xv.wait()
    os.environ["XRES"] = "3840x2160x24"
    xv = start_xvfb()
    p = run()
    wid = subprocess.run(["xdotool", "search", "--name", "xtty demo"], capture_output=True, text=True).stdout.split()[0]
    check("4K starts at size 7 (31x62 cells): 34 x 123 cells", log().splitlines()[0] == "size 34x123", log())
    xdo("windowsize", wid, "3840", "2160"); time.sleep(.8)
    for _ in range(5): xdo("key", "ctrl+minus"); time.sleep(.4)
    check("five Ctrl+-: size 2 (13x26 cells): 83 x 295 cells", last() == "size 83x295", log())
    n = count()
    xdo("key", "ctrl+minus"); time.sleep(.6)
    check("Ctrl+- again, to size 1, is refused: nothing changes", count() == n, log())
    check("the window stays 3840x2160", winsize() == "3840x2160", winsize())
    for _ in range(5): xdo("key", "ctrl+equal"); time.sleep(.4)
    check("five Ctrl+=: back to size 7: 34 x 123 cells", last() == "size 34x123", log())
    p.kill(); p.wait()

    # The start size: the grid nearest 30 lines on the screen (XTERM_START_LINES).
    print("## the start size: the grid nearest 30 lines on the screen")
    for screen, want in [("640x480", "size 26x71"), ("800x600", "size 27x72"),
                         ("1024x768", "size 29x78"), ("1920x1080", "size 29x106"),
                         ("2560x1440", "size 27x98"), ("3840x2160", "size 34x123")]:
        xv.kill(); xv.wait()
        os.environ["XRES"] = screen + "x24"
        xv = start_xvfb()
        p = run()
        check("%s starts at %s" % (screen, want), log().splitlines()[0] == want, log())
        p.kill(); p.wait()
finally:
    xv.kill()
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail)); sys.exit(1 if fail else 0)
