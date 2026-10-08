import sys, os, time, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
# mc_lib.py (alongside): PlainTmux, start_xvfb, start_xtmux, xdo, shot
from mc_lib import *   # noqa: sibling module
CW, CH, OX, OY = 13, 26, 3, 1      # cell size; window origin (no WM): window at +3,+1
def cell_xy(c, r): return (OX + c*CW + CW//2, OY + r*CH + CH//2)
def move(c, r): x, y = cell_xy(c, r); xdo("mousemove", str(x), str(y)); time.sleep(0.08)
def click(c, r, b=1): move(c, r); xdo("click", str(b)); time.sleep(0.12)
def log(): 
    try: return open("/tmp/mouse.log", "rb").read()
    except FileNotFoundError: return b""
def clear(): open("/tmp/mouse.log", "wb").close()
def start_logger(xt_modes, modes):
    for f in ("/tmp/mouse.ready", "/tmp/mouse.log"):
        try: os.unlink(f)
        except FileNotFoundError: pass
    xdo("type", "python3 %s " % os.path.join(HERE, "mouse_log.py") + " ".join(modes)); xdo("key", "Return")
    for _ in range(40):
        if os.path.exists("/tmp/mouse.ready"): break
        time.sleep(0.1)
    time.sleep(0.5); clear()
def stop_logger(): xdo("key", "ctrl+q"); time.sleep(0.4)

# ---- check()/reset() shared by the tests

results = []
def check(name, ok, extra=""):
    results.append(ok); print("  %-4s %s %s" % ("PASS" if ok else "FAIL", name, extra))
def sh(cmd): return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout
def reset():
    sh("pkill -9 -x tmux; pkill -9 -x xtmux; pkill -9 -x openbox; pkill -9 Xvfb; rm -f /run/utmux /tmp/mouse.log /tmp/mouse.ready"); time.sleep(1)
