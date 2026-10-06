#!/bin/bash
# usage: xtmux_scrollback_test.sh XTMUX_BINARY  (needs Xvfb, xdotool, ImageMagick)
# Types seq 1 100, enters the viewer with Ctrl+B PgUp, then Up/PgDn/Esc; leaves screenshots /tmp/sb_{live,pgup,up2,pgdn,esc}.png
# (esc must be pixel-identical to live).
setsid Xvfb :2 -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 </dev/null &
sleep 1; export DISPLAY=:2
setsid bash -c "sleep 120 | $1 2>/tmp/xtmux.err" >/dev/null 2>&1 </dev/null &
sleep 1.5
XP=$(pgrep -x xtmux | head -1); [ -n "$XP" ] || { echo SETUP FAILED; pkill Xvfb; exit 1; }
xdotool type "seq 1 100"; xdotool key Return; sleep 0.8
xwd -root -silent | convert xwd:- /tmp/sb_live.png
xdotool key ctrl+b; sleep 0.2; xdotool key Prior; sleep 0.8
xwd -root -silent | convert xwd:- /tmp/sb_pgup.png
xdotool key Up; sleep 0.4; xdotool key Up; sleep 0.4
xwd -root -silent | convert xwd:- /tmp/sb_up2.png
xdotool key Next; sleep 0.5
xwd -root -silent | convert xwd:- /tmp/sb_pgdn.png
xdotool key Escape; sleep 0.8
xwd -root -silent | convert xwd:- /tmp/sb_esc.png
echo "alive after sequence: $(kill -0 $XP 2>/dev/null && echo yes || echo NO)  stderr=[$(cat /tmp/xtmux.err)]"
pkill -x xtmux; pkill Xvfb; sleep 1; rm -f /tmp/.X2-lock /tmp/.X11-unix/X2
