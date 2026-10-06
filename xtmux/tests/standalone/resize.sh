#!/bin/bash
# usage: resize_test.sh xtmux-binary [wm]  -> prints expected vs actual `stty size` (rows cols) after each resize
setsid Xvfb :2 -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 </dev/null &
sleep 1; export DISPLAY=:2
[ "${2:-openbox}" != none ] && { setsid openbox >/dev/null 2>&1 </dev/null & sleep 1; }
setsid bash -c "sleep 120 | $1 2>/tmp/xtmux.err" >/dev/null 2>&1 </dev/null &
sleep 1.5
XP=$(pgrep -x xtmux | head -1); WID=$(xdotool search --name u-tmux | head -1)
check() {   # $1 w $2 h : expected child rows/cols from the *actual* window geometry
  [ -n "$1" ] && { xdotool windowsize $WID $1 $2; sleep 0.6; }
  read -r aw ah < <(xdotool getwindowgeometry $WID | awk '/Geometry/{split($2,a,"x"); print a[1], a[2]}')
  ec=$((aw/13)); er=$((ah/26)); [ $ec -lt 1 ] && ec=1; [ $er -lt 1 ] && er=1
  er=$((er>1?er-1:1))
  rm -f /tmp/size.txt; xdotool type "stty size > /tmp/size.txt"; xdotool key Return; sleep 0.6
  got=$(cat /tmp/size.txt 2>/dev/null)
  alive=yes; kill -0 $XP 2>/dev/null || alive=NO
  printf "window %4sx%-4s -> expected '%s %s'  pty says '%s'  alive=%s %s\n" $aw $ah $er $ec "$got" $alive "$([ "$got" = "$er $ec" ] && echo OK || echo MISMATCH)"
}
check
for sz in "400 300" "800 600" "200 100" "13 26" "1 1" "30 600" "600 30" "790 590"; do check $sz; done
# burst: many resizes in quick succession, then settle
for i in $(seq 1 25); do xdotool windowsize $WID $((300+i*15)) $((200+i*10)); done; sleep 1; check
kill -0 $XP 2>/dev/null && echo "stderr: [$(cat /tmp/xtmux.err)]"
xwd -root -silent | convert xwd:- /tmp/resize_end.png 2>/dev/null
pkill -x xtmux; pkill openbox; pkill Xvfb; sleep 0.5
