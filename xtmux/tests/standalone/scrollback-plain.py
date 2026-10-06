#!/usr/bin/env python3
# usage: plain_scrollback_test.py TMUX_BINARY  (needs python3-pyte) -- plain tmux in a pty+terminal emulator;
# dumps the screen after ^B PgUp / Up. Footer must read " SCROLLBACK   N/M lines  (q/Esc to exit)".
import os, pty, sys, time, select, struct, fcntl, termios, pyte
binary = sys.argv[1]; ROWS, COLS = 24, 80
screen = pyte.Screen(COLS, ROWS); stream = pyte.ByteStream(screen)
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"; os.execv(binary, [binary])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
def pump(t=0.6):
    end = time.time() + t
    while time.time() < end:
        r,_,_ = select.select([fd],[],[],0.05)
        if r:
            try: data = os.read(fd, 65536)
            except OSError: return
            if not data: return
            stream.feed(data)
def dump(title):
    print(f"--- {title}")
    for i, line in enumerate(screen.display): print(f"{i:2d}|{line.rstrip()}")
pump(1.0)
os.write(fd, b"seq 1 100\n"); pump(1.0)
dump("before scrollback (tail)")
os.write(fd, b"\x02"); pump(0.2); os.write(fd, b"\x1b[5~"); pump(0.8)
dump("after ^B PgUp")
for c in (0, 3, 20, 40, 79):
    ch = screen.buffer[ROWS-1][c]; print("  footer col %2d %r fg=%s bg=%s" % (c, ch.data, ch.fg, ch.bg))
os.write(fd, b"\x1b[A"); pump(0.5)
dump("after Up")
os.write(fd, b"q"); pump(0.5)
os.write(fd, b"\x02d") if False else None
os.kill(pid, 9)
