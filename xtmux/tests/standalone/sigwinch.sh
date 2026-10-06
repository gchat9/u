#!/bin/bash
# usage: xtmux_sigwinch_test.sh XTMUX_BINARY  (needs Xvfb, xdotool)
# Freezes the X server so xtmux blocks in write(), then sends SIGWINCH. Run it against an xtmux whose
# SIGWINCH handler lacks SA_RESTART (dietlibc builds) to see "xwrite failed" without patch 0006.
setsid Xvfb :2 -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 </dev/null &
sleep 1; export DISPLAY=:2
XV=$(pgrep -x Xvfb | head -1)
setsid bash -c "sleep 120 | $1 2>/tmp/xtmux.err" >/dev/null 2>&1 </dev/null &
sleep 1.5
XP=$(pgrep -x xtmux | head -1)
[ -n "$XP" ] || { echo "SETUP FAILED"; pkill Xvfb; exit 1; }
xdotool type "sleep 2; seq 1 200"; xdotool key Return
sleep 0.5
kill -STOP $XV                      # server frozen; output arrives ~1.5s later
sleep 3
st=$(cut -d' ' -f1 /proc/$XP/syscall 2>/dev/null); echo "xtmux syscall while server frozen: $st ($( [ "$st" = 1 ] && echo 'write: blocked' || echo 'not in write'))"
kill -WINCH $XP; sleep 0.3
kill -CONT $XV; sleep 1
if kill -0 $XP 2>/dev/null; then echo "$1: survived SIGWINCH during blocked write"; else echo "$1: DIED: [$(cat /tmp/xtmux.err)]"; fi
kill -CONT $XV 2>/dev/null; pkill -x xtmux; pkill Xvfb; sleep 1
