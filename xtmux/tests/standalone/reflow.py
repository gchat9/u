#!/usr/bin/env python3
# usage: reflow.py XTMUX  -- a 100-character line, then window resizes: wider (one row
# holds it), then narrower (it spills over three rows).  Checks the pixels of each
# text row; the line is zeros, the prompt is short.  Needs Xvfb, xdotool, python3-pillow.
# Header comment style as the other standalone checks: prints what it sees and OK/MISMATCH.
import sys, os, time, subprocess
from PIL import Image
XT = sys.argv[1]
CW, CH, OX, OY = 13, 26, 0, 0      # cell origin in this window; glyph ink sits 15-30 px down
def sh(cmd): return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout
subprocess.run("pkill -x xtmux; pkill Xvfb", shell=True); time.sleep(0.5)
subprocess.Popen("Xvfb :2 -screen 0 1600x800x24 -nolisten tcp", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1); os.environ["DISPLAY"] = ":2"
subprocess.Popen("sleep 120 | %s 2>/tmp/reflow.err" % XT, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)
WID = sh("xdotool search --name u-tmux | head -1").strip()
subprocess.run(["xdotool", "windowsize", WID, "800", "600"]); time.sleep(1.5)     # 61 columns to start
def xdo(*a): subprocess.run(["xdotool", *a])
def shot(p):
    subprocess.run("xwd -root -silent | convert xwd:- %s" % p, shell=True); return Image.open(p).convert("RGB")
def lit(im, row, c0, c1):
    """any text-bright pixel in text row `row`, columns [c0, c1)"""
    x0, x1 = OX + c0 * CW, OX + c1 * CW
    y0, y1 = OY + row * CH + 8, OY + row * CH + 24     # the middle of the row, clear of the overhang
    return any(max(im.getpixel((x, y))) > 120 for x in range(x0, min(x1, im.width)) for y in range(y0, y1))
results = []
def check(label, ok, got):
    results.append(ok); print("%-62s %s  %s" % (label, "OK" if ok else "MISMATCH", got))
xdo("type", "clear; printf '%0100d\\n' 0"); xdo("key", "Return"); time.sleep(1)
im = shot("/tmp/reflow0.png")
check("61 columns: the line starts row 0 and wraps to row 1", lit(im, 0, 0, 61) and lit(im, 1, 0, 39) and not lit(im, 1, 40, 61),
      "row0 full, row1 39 cells")
subprocess.run(["xdotool", "windowsize", WID, "1400", "600"]); time.sleep(1.5)     # 107 columns
im = shot("/tmp/reflow1.png")
check("107 columns: the line is one row, the prompt moved up", lit(im, 0, 62, 100) and not lit(im, 1, 50, 100),
      "row0 reaches col 100, row1 quiet past col 50 (the prompt is 48 wide)")
subprocess.run(["xdotool", "windowsize", WID, "520", "600"]); time.sleep(1.5)      # 40 columns
im = shot("/tmp/reflow2.png")
check("40 columns: three rows, 40 + 40 + 20 zeros", lit(im, 0, 0, 40) and lit(im, 1, 0, 40) and lit(im, 2, 0, 20) and not lit(im, 2, 21, 40),
      "rows 0-1 full, row 2 has 20")
subprocess.run("pkill -x xtmux; pkill Xvfb", shell=True)
print("%d/%d OK" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
