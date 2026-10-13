#!/usr/bin/env python3
# Colours of xtmux, sampled from a screenshot: the default background and
# foreground are palette slots 0 and 7 (#1a1a1a, #cccccc); slot 15 is #ffffff
# and renders as such; bold default text is slot 15; the 6x6x6 cube and the
# greyscale ramp are as listed.  Usage: python3 xtmux/tests/palette.py
# Needs Xvfb, xdotool, xwd + ImageMagick, python3 with pillow; run as root (or
# with a writable /run/utmux).  Build xtmux/ first (or set XTMUX_BIN).
import sys, os, time, subprocess
TESTS = os.path.dirname(os.path.abspath(__file__))
XTMUX = os.environ.get("XTMUX_BIN", os.path.normpath(os.path.join(TESTS, "..", "xtmux")))
sys.path.insert(0, os.path.join(TESTS, "mouse"))
from mouse_lib import *            # reset, check, results, xdo, shot, start_xvfb, start_xtmux, CW, CH, OX, OY
from PIL import Image

# (label, SGR that colours a 5-cell reverse or background run, expected #rrggbb)
CASES = [
    ("default fg, reversed (-> bg)",      "\\033[7m     \\033[0m",         "cccccc"),
    ("slot 15 as bg (107)",               "\\033[107m     \\033[0m",       "ffffff"),
    ("slot 15 as fg, reversed (97;7)",    "\\033[97;7m     \\033[0m",      "ffffff"),
    ("slot 0 as bg (40)",                 "\\033[40m     \\033[0m",        "1a1a1a"),
    ("bold default fg, reversed",         "\\033[1;7m     \\033[0m",       "ffffff"),
    ("slot 7 as bg (47)",                 "\\033[47m     \\033[0m",        "cccccc"),
    ("slot 8 as bg (100)",                "\\033[100m     \\033[0m",       "666666"),
    ("slot 9 as bg (101)",                "\\033[101m     \\033[0m",       "ff0000"),
    ("cube 16 as bg (48;5;16)",           "\\033[48;5;16m     \\033[0m",    "000000"),
    ("cube 196 as bg (48;5;196)",         "\\033[48;5;196m     \\033[0m",   "ff0000"),
    ("cube 231 as bg (48;5;231)",         "\\033[48;5;231m     \\033[0m",   "ffffff"),
    ("greyscale 232 as bg (48;5;232)",    "\\033[48;5;232m     \\033[0m",   "080808"),
]
DEFAULT_BG = "1a1a1a"      # a plain cell, row 0

def px(im, col, row):
    return "%02x%02x%02x" % im.getpixel((OX + col*CW + CW//2, OY + row*CH + CH//2))

reset(); os.environ["XRES"] = "800x600x24"
xv = start_xvfb(); xt = start_xtmux(XTMUX); time.sleep(2)
# row 0 stays plain; each case then gets its own row, 1 onward
cmd = "clear; printf '\\n'; " + "; ".join("printf '%s\\n'" % esc for _, esc, _ in CASES)
xdo("type", cmd); xdo("key", "Return"); time.sleep(1.5)
shot("/tmp/palette.png")
im = Image.open("/tmp/palette.png").convert("RGB")
check("a plain cell's background is slot 0 (#1a1a1a)", px(im, 40, 0) == DEFAULT_BG, px(im, 40, 0))
for i, (label, _, want) in enumerate(CASES):
    got = px(im, 2, i + 1)
    check("%-34s %s" % (label, "#" + want), got == want, "got #" + got)
xt.kill(); xv.kill(); subprocess.run("pkill -9 -x xtmux; pkill -9 Xvfb", shell=True)
print("\nSUMMARY: %d/%d passed" % (sum(results), len(results)))
