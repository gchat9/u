#!/bin/bash
# usage: xtmux_bold_test.sh XTMUX_BINARY [DEPTH]  (needs Xvfb, xdotool, ImageMagick, python3-pil)
# Prints the brightest pixel of "normal" vs bold "bright" text; expect #ffffff for the latter (patch 0007).
D=${2:-24}
setsid Xvfb :2 -screen 0 800x600x$D -nolisten tcp >/dev/null 2>&1 </dev/null &
sleep 1; export DISPLAY=:2
setsid bash -c "sleep 60 | $1 2>/tmp/xtmux.err" >/dev/null 2>&1 </dev/null &
sleep 1.5
xdotool type "printf '\\033[2J\\033[Hnormal \\033[1mbright\\033[22m\\n'"; xdotool key Return; sleep 0.8
xwd -root -silent > /tmp/bold.xwd; convert xwd:/tmp/bold.xwd /tmp/bold_d$D.png
python3 - "$D" <<'PY'
import sys
from PIL import Image
d = sys.argv[1]
im = Image.open("/tmp/bold_d%s.png" % d).convert("RGB")
# window is at (3,1) with no WM; row 0 = y 1..26 ; "normal " = cells 0-6, "bright" = cells 7-12
def stats(c0, c1):
    cols = {}
    for y in range(1, 27):
        for x in range(3 + c0*13, 3 + c1*13):
            p = im.getpixel((x, y)); cols[p] = cols.get(p, 0) + 1
    return cols
n, b = stats(0, 7), stats(7, 13)
hexc = lambda c: "#%02x%02x%02x" % c
print("d%s: 'normal' brightest px %s (pure-white px: %d) | 'bright' brightest px %s (pure-white px: %d)" %
      (d, hexc(max(n)), n.get((255,255,255),0), hexc(max(b)), b.get((255,255,255),0)))
PY
pkill -x xtmux; pkill Xvfb; sleep 1; rm -f /tmp/.X2-lock /tmp/.X11-unix/X2
