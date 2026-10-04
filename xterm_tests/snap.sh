#!/bin/bash
# Usage: ./snap.sh [depth] -- prints a short hash of the screen after each of 6 test frames.
# Run it with xt_test built from two different versions of xterm.c; identical output
# means identical pixels. Needs Xvfb.
D=${1:-24}; FB=$(mktemp -d)
Xvfb :2 -screen 0 640x480x$D -fbdir $FB -nolisten tcp >/dev/null 2>&1 & XP=$!
sleep 1; export DISPLAY=:2
for n in 0 1 2 3 4 5; do
    ./xt_test $n & BP=$!; sleep 0.8
    printf '%s ' "$(md5sum $FB/Xvfb_screen0 | cut -c1-8)"; wait $BP
done; echo
kill $XP; rm -rf $FB
