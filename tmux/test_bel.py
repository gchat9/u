#!/usr/bin/env python3
# Tests tmux's BEL (the VT build) through a pty:  python3 tmux/test_bel.py [path/to/tmux]
# (default: ./tmux next to this file).  A BEL in a pane, or in a background window,
# rings the terminal tmux runs in; several in one read make one.  A background
# window that rang is marked in the status bar (window-status-activity-style:
# fg 235 on bg 78) until it is made current, and the mark survives a detach and
# attach.  Needs only python3.
import fcntl, os, pty, select, signal, struct, sys, termios, time
HERE = os.path.dirname(os.path.abspath(__file__))
TMUX = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "tmux")
MARK_FG, MARK_BG = b"38;5;235", b"48;5;78"       # the activity style of the status bar
def drain(fd, secs):
    out = b""; end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: break
    return out
def start(args):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM="xterm-256color", SHELL="/bin/sh", HOME="/tmp")
        os.execv(TMUX, [TMUX] + args)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    return pid, fd
def reap(pid):
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
ok = True
def check(name, cond, detail=""):
    global ok; ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + detail))
pid, fd = start([])
drain(fd, 1.5)
quiet = drain(fd, 0.5)
check("quiet before any bell (tmux itself rings nothing)", b"\x07" not in quiet, repr(quiet[-200:]))
os.write(fd, b"printf 'one\\a'; echo MARK1\n")
out = drain(fd, 1.5)
check("BEL in the current pane rings the terminal", out.count(b"\x07") >= 1, repr(out[-300:]))
check("BEL in the current pane does not mark its window", MARK_BG not in out, repr(out[-300:]))
check("several BELs in one read make one", out.count(b"\x07") == 1, "got %d" % out.count(b"\x07"))
os.write(fd, b"(sleep 1; printf 'bg\\a') &\n")          # a BEL from window 0 ...
drain(fd, 0.3)
os.write(fd, b"\x02c")                                   # ... after window 1 is made current
out = drain(fd, 2.5)
check("BEL in a background window rings the terminal", b"\x07" in out, repr(out[-300:]))
check("... and marks its tab (fg 235 on bg 78)", MARK_FG in out and MARK_BG in out, repr(out[-300:]))
os.write(fd, b"\x02d")                                   # detach: the mark is kept by the daemon
drain(fd, 1.0); reap(pid)
pid2, fd2 = start(["attach"])
out = drain(fd2, 2.5)
check("the mark survives a detach and attach", MARK_BG in out, repr(out[-300:]))
os.write(fd2, b"\x021")                                  # make window 0 current: the mark goes
out = drain(fd2, 1.5)
check("made current, the mark goes", MARK_BG not in out and MARK_FG not in out, repr(out[-300:]))
os.write(fd2, b"\x02q"); drain(fd2, 0.5); reap(pid2)
print("SUMMARY: %s" % ("all passed" if ok else "FAILURES"))
sys.exit(0 if ok else 1)
