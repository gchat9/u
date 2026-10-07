#!/usr/bin/env python3
# usage: mouse_log.py MODE... -- enable the given DEC private modes (e.g. 1000 1006), put the tty in raw mode and append every
# byte received on stdin to /tmp/mouse.log until 'q' is typed ('d' / 'e' disable / re-enable the modes).  "ready" is written to /tmp/mouse.ready once enabled.
import os, sys, tty, termios
modes = sys.argv[1:]
fd = 0; old = termios.tcgetattr(fd); tty.setraw(fd)
log = os.open("/tmp/mouse.log", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
os.write(1, "".join("\x1b[?%sh" % m for m in modes).encode()); 
open("/tmp/mouse.ready", "w").write("1")
try:
    while True:
        d = os.read(0, 4096)
        if not d or d == b"q": break
        if d == b"d": os.write(1, "".join("\x1b[?%sl" % m for m in modes).encode()); continue   # 'd': disable
        if d == b"e": os.write(1, "".join("\x1b[?%sh" % m for m in modes).encode()); continue   # 'e': enable again
        os.write(log, d)
finally:
    os.write(1, "".join("\x1b[?%sl" % m for m in reversed(modes)).encode())
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
