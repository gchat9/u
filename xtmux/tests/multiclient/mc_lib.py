# Shared helpers for multi-client tests: a plain tmux running in a pty with a pyte terminal emulator, plus xtmux on Xvfb.
import os, pty, sys, time, select, struct, fcntl, termios, subprocess, signal, pyte
class PlainTmux:
    """plain tmux (or `tmux attach`) in a pty; screen mirrored in pyte."""
    def __init__(self, binary, args=(), rows=24, cols=80):
        self.rows, self.cols = rows, cols
        self.screen = pyte.Screen(cols, rows); self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"; os.environ["PS1"] = "$ "
            os.execv(binary, [binary, *args])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.exited = False
    def pump(self, t=0.6):
        end = time.time() + t
        while time.time() < end:
            r,_,_ = select.select([self.fd],[],[],0.05)
            if r:
                try: d = os.read(self.fd, 65536)
                except OSError: self.exited = True; return
                if not d: self.exited = True; return
                self.stream.feed(d)
    def send(self, b): os.write(self.fd, b)
    def text(self): return "\n".join(l.rstrip() for l in self.screen.display)
    def alive(self):
        try: p,_ = os.waitpid(self.pid, os.WNOHANG); return p == 0
        except ChildProcessError: return False
    def kill(self):
        try: os.kill(self.pid, 9); os.waitpid(self.pid, 0)
        except Exception: pass
def start_xvfb():
    for f in ("/tmp/.X2-lock", "/tmp/.X11-unix/X2"):
        try: os.unlink(f)
        except FileNotFoundError: pass
    p = subprocess.Popen(["Xvfb", ":2", "-screen", "0", os.environ.get("XRES","800x600x24"), "-nolisten", "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1); os.environ["DISPLAY"] = ":2"; return p
class XTmuxProc:
    """xtmux launched like from a terminal: stdio on a pty of the given size (default 40x120, unlike the X grid)."""
    def __init__(self, binary, args=(), rows=40, cols=120):
        m, sl = pty.openpty()
        fcntl.ioctl(sl, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.master = m; self.out = b""
        self.p = subprocess.Popen([binary, *args], stdin=sl, stdout=sl, stderr=sl, start_new_session=True,
                                  preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(sl)
    def drain(self):
        while True:
            r,_,_ = select.select([self.master],[],[],0)
            if not r: break
            try: d = os.read(self.master, 65536)
            except OSError: break
            if not d: break
            self.out += d
    def poll(self): return self.p.poll()
    def kill(self, sig=9):
        try: self.p.send_signal(sig); 
        except Exception: pass
        try: self.p.wait(timeout=3)
        except Exception: pass
    def tty_text(self):
        self.drain(); return self.out.decode("latin1")
def start_xtmux(binary, args=(), **kw): return XTmuxProc(binary, args, **kw)
def xdo(*a): subprocess.run(["xdotool", *a], stderr=subprocess.DEVNULL)
def shot(path): subprocess.run("xwd -root -silent | convert xwd:- %s" % path, shell=True)
