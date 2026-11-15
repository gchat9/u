#!/usr/bin/env python3
# usage: bell.py XTMUX  -- a BEL in a pane marks the xtmux window urgent (WM_HINTS) only
# while the window lacks focus; gaining focus clears the mark.  A background window
# that rang is marked in the status bar (activity style: bg 78) until made current.
# Needs DISPLAY (an X server, no window manager needed), xdotool, xprop/xwininfo
# (x11-utils) and xwd (x11-apps).
import os, struct, subprocess, sys, tempfile, time
BIN = sys.argv[1]
def sh(*a): return subprocess.run(list(a), capture_output=True, text=True).stdout
def wm_hints(w): return sh("xprop", "-id", w, "WM_HINTS")
def urgent(w): return "urgency hint bit is set" in wm_hints(w)
def xd(*a): subprocess.run(["xdotool", *a])
def marked(w):
    # The mark is a solid block of the activity background (bg 78 = #5fd787) across the
    # status bar; text in that colour (the hostname, the clock) is short strokes, so the
    # longest run along the middle of the bar tells them apart.  XWD: big-endian
    # header; 32bpp pixels, in either channel order.
    path = os.path.join(tempfile.mkdtemp(prefix="xtmux-bell-"), "win.xwd")
    subprocess.run(["xwd", "-id", w, "-silent", "-out", path], check=True)
    d = open(path, "rb").read()
    hsize, = struct.unpack(">I", d[0:4]); bpp, = struct.unpack(">I", d[44:48])
    bpl, = struct.unpack(">I", d[48:52]); ncol, = struct.unpack(">I", d[76:80])
    W, H = struct.unpack(">II", d[80:88]); px = d[hsize + ncol * 12:]
    if bpp != 32: return False
    want = {bytes((95, 215, 135)), bytes((135, 215, 95))}
    y = H - 12                                     # middle of the bottom (status) row
    run = best = 0
    for x in range(W):
        if px[y * bpl + x * 4: y * bpl + x * 4 + 3] in want:
            run += 1; best = max(best, run)
        else:
            run = 0
    return best >= 20
root = sh("xwininfo", "-root").split("Window id:")[1].split()[0]
env = dict(os.environ, SHELL="/bin/sh", HOME="/tmp")
p = subprocess.Popen([BIN], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
ok = True
def check(name, cond, detail=""):
    global ok; ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + detail))
try:
    w = None
    for _ in range(50):
        for line in sh("xwininfo", "-root", "-tree").splitlines():
            if '"u-tmux"' in line: w = line.split()[0]
        if w: break
        time.sleep(0.2)
    check("xtmux window found", w is not None)
    if w is None: sys.exit(1)   # no window: nothing to test (the check above says so)
    time.sleep(1); xd("windowfocus", "--sync", w); time.sleep(0.5)
    check("no WM_HINTS before any bell", not urgent(w), wm_hints(w))
    xd("type", "--delay", "20", "(sleep 1; printf '\\a') &"); xd("key", "Return")
    time.sleep(2.5)
    check("BEL while focused does not mark the window", not urgent(w), wm_hints(w))
    xd("type", "--delay", "20", "(sleep 1; printf '\\a') &"); xd("key", "Return")
    xd("windowfocus", "--sync", root); time.sleep(2.5)
    check("BEL while unfocused marks the window urgent", urgent(w), wm_hints(w))
    xd("windowfocus", "--sync", w); time.sleep(0.8)
    check("gaining focus clears the urgency mark", not urgent(w), wm_hints(w))
    # The status bar: window 0 rings while window 1 is current, so its tab is marked.
    xd("type", "--delay", "20", "(sleep 1; printf '\\a') &"); xd("key", "Return"); time.sleep(0.5)
    xd("key", "ctrl+b"); xd("key", "c"); time.sleep(2.5)
    check("a background window that rang is marked in the status bar", marked(w))
    xd("key", "ctrl+b"); xd("key", "1"); time.sleep(1.0)
    check("made current, the mark goes", not marked(w))
finally:
    p.kill(); p.wait()
sys.exit(0 if ok else 1)
