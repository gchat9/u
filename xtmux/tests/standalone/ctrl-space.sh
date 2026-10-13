#!/bin/bash
# usage: ctrl-space.sh XTMUX_BINARY  (needs Xvfb, xdotool, python3-xlib)
# Sends Space, then Ctrl+Space, to the window and prints the bytes the shell
# in it receives for each, in hex.  Expect "20" for Space and "00" for Ctrl+Space
# (NUL, as xterm sends it).  Before the fix, Ctrl+Space also gave "20".
DIR=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
D=:9

# The shell xtmux runs in the window: raw mode, logs each byte it reads as hex.
cat > "$T/keylog" <<EOF
#!/usr/bin/env python3
import os, tty
tty.setraw(0)
log = open("$T/keys", "a", buffering=1)
while True:
    b = os.read(0, 1)
    if not b:
        break
    log.write("%02x\n" % b[0])
EOF
chmod +x "$T/keylog"

cleanup() {
    pkill -x xtmux 2>/dev/null
    pkill -f "$T/keylog" 2>/dev/null
    [ -n "$XV" ] && kill "$XV" 2>/dev/null
    [ -n "$WM" ] && kill "$WM" 2>/dev/null
    rm -rf "$T" /tmp/.X9-lock /tmp/.X11-unix/X9 /run/utmux-root 2>/dev/null
}
trap cleanup EXIT

Xvfb $D -screen 0 640x480x24 -nolisten tcp >/dev/null 2>&1 &
XV=$!
sleep 1
export DISPLAY=$D
python3 "$DIR/../_zerowm.py" >/dev/null 2>&1 &
WM=$!
sleep 1
SHELL="$T/keylog" setsid "$1" >"$T/xtmux.log" 2>&1 </dev/null &
sleep 2

WID=$(xdotool search --name '^u-tmux$' 2>/dev/null | head -1)
[ -n "$WID" ] || { echo "SETUP FAILED: no window"; cat "$T/xtmux.log"; exit 1; }
xdotool windowfocus --sync "$WID" 2>/dev/null
xdotool mousemove --window "$WID" 100 100
sleep 0.5

count() { cat "$T/keys" 2>/dev/null | wc -l; }
for k in space ctrl+space; do
    n0=$(count)
    xdotool key "$k"
    sleep 0.8
    got=$(sed -n "$((n0 + 1)),\$p" "$T/keys" 2>/dev/null | tr '\n' ' ')
    printf '%-12s -> [%s]\n' "$k" "${got% }"
done
