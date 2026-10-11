#!/usr/bin/env python3
# Tests xmssh against a local sshd, on Xvfb:  python3 xmssh/test.py
# Needs sshd (OpenSSH server), ssh-keygen, Xvfb, xdotool, xclip, python-xlib,
# and /etc/font.bfnt (make -C _font install); the helpers of xtmux/tests/mouse.
import os, sys, subprocess, tempfile, time, shutil
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "xtmux", "tests", "mouse"))
from mouse_lib import *
from Xlib import display, X, protocol

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xmssh")
PORT = "2299"
T = tempfile.mkdtemp(prefix="xmssh-test-")
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1
    else: fail += 1
    print("  %s %s %s" % ("PASS" if cond else "FAIL", name, "" if cond else info))
def xc(sel): return subprocess.run(["xclip", "-o", "-selection", sel], capture_output=True, timeout=5).stdout
def own(sel, data): subprocess.run(["xclip", "-i", "-selection", sel], input=data, timeout=5); time.sleep(.3)
def wait(cond, t=8):
    end = time.time() + t
    while time.time() < end:
        if cond(): return True
        time.sleep(.1)
    return cond()
def rd(f):
    try: return open(os.path.join(T, f)).read().strip()
    except OSError: return None
def typed(s): xdo("type", "--delay", "30", s); xdo("key", "Return")
def window(p):
    """The window of process p: the newest one titled user@host (we set no _NET_WM_PID)."""
    out = []
    wait(lambda: out.append(subprocess.run(["xdotool", "search", "--name", "root@127.0.0.1"], capture_output=True, text=True).stdout.split()) or out[-1])
    return out[-1][-1] if out[-1] else None
def close_window(wid):
    d = display.Display(); win = d.create_resource_object("window", int(wid)); a = d.intern_atom
    win.send_event(protocol.event.ClientMessage(window=win, client_type=a("WM_PROTOCOLS"),
                   data=(32, [a("WM_DELETE_WINDOW"), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.flush(); d.sync(); d.close()
def start(*args, kh="known_hosts"):
    return subprocess.Popen([BIN, "-o", "UserKnownHostsFile=" + os.path.join(T, kh), "-i", os.path.join(T, "key"),
                             "-p", PORT, "root@127.0.0.1", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# ---- a sshd of our own, a key, and a known host
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", os.path.join(T, "key")], check=True)
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", os.path.join(T, "hostkey")], check=True)
shutil.copy(os.path.join(T, "key.pub"), os.path.join(T, "authorized_keys"))
open(os.path.join(T, "sshd_config"), "w").write("""Port %s
ListenAddress 127.0.0.1
HostKey %s/hostkey
AuthorizedKeysFile %s/authorized_keys
PermitRootLogin yes
PasswordAuthentication no
UsePAM no
StrictModes no
PidFile %s/sshd.pid
""" % (PORT, T, T, T))
os.makedirs("/run/sshd", exist_ok=True)
sshd = subprocess.Popen(["/usr/sbin/sshd", "-f", os.path.join(T, "sshd_config"), "-D", "-e"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
hk = subprocess.run(["ssh-keygen", "-y", "-f", os.path.join(T, "hostkey")], capture_output=True, text=True).stdout.split()
open(os.path.join(T, "known_hosts"), "w").write("127.0.0.1 %s %s\n" % (hk[0], hk[1]))   # for the tests that are not about this

xv = start_xvfb()
try:
    print("## a session in a window")
    p = start(); wid = window(p)
    check("the window opens, titled user@host", wid and sh("xdotool getwindowname %s" % wid).strip() == "root@127.0.0.1", sh("xdotool getwindowname %s" % wid))
    time.sleep(2)
    typed("echo marker-$((6*7)) > %s/m1" % T)
    check("typing reaches the remote shell", wait(lambda: rd("m1") == "marker-42"), rd("m1"))
    geo = sh("xdotool getwindowgeometry %s" % wid).split("Geometry:")[1].split()[0].split("x")
    cols, rows = (int(geo[0]) - 3) // CW if False else int(geo[0]) // CW, int(geo[1]) // CH
    typed("stty size > %s/s1" % T)
    check("the pty has the window's size (%dx%d cells)" % (cols, rows), wait(lambda: rd("s1") == "%d %d" % (rows, cols)), rd("s1"))
    xdo("windowsize", wid, "520", "260"); time.sleep(1)
    typed("stty size > %s/s2" % T)
    check("resizing the window sends a window-change", wait(lambda: rd("s2") == "10 40"), rd("s2"))
    typed("clear; echo hello-copy-test"); time.sleep(1)
    own("primary", b"sentinel"); mclick = lambda c, r, n: [move(c, r)] + [xdo("click", "1") or time.sleep(.05) for _ in range(n)] + [time.sleep(.3)]
    mclick(3, 0, 2)                                   # (after clear, the output is on row 0)
    check("double click copies a word out of the window", xc("primary") == b"hello-copy-test", xc("primary"))
    own("primary", b"pasted-text"); typed("echo -n 'x' > /dev/null; true"); time.sleep(.5)
    xdo("type", "--delay", "30", "echo "); xdo("key", "ctrl+v"); time.sleep(.6); xdo("type", "--delay", "30", " > %s/p1" % T); xdo("key", "Return")
    check("Ctrl+V pastes into the remote shell", wait(lambda: rd("p1") == "pasted-text"), rd("p1"))
    typed("exit 3")
    check("exit closes the window and the program exits with the remote status", wait(lambda: p.poll() == 3), p.poll())
    if p.poll() is None: p.kill()

    print("## a command")
    p = start("echo cmd-out; exit 5"); wid = window(p); time.sleep(2.5)
    check("a command's output stays up after it ends", p.poll() is None)
    close_window(wid)
    check("closing the window ends the program with the command's status", wait(lambda: p.poll() == 5), p.poll())
    if p.poll() is None: p.kill()

    print("## the host key question, in the window")
    kh = "new_hosts"
    p = start("echo ok > %s/h1" % T, kh=kh); wid = window(p); time.sleep(2)
    xdo("type", "--delay", "40", "yes"); xdo("key", "BackSpace"); xdo("type", "--delay", "40", "s"); xdo("key", "Return")
    check("answering yes (with a correction) connects and remembers the host",
          wait(lambda: rd("h1") == "ok") and "ssh-ed25519" in (rd(kh) or ""), (rd("h1"), rd(kh)))
    wait(lambda: p.poll() is not None or True, 1)
    if p.poll() is None: close_window(wid)
    wait(lambda: p.poll() is not None); 
    os.unlink(os.path.join(T, kh)) if os.path.exists(os.path.join(T, kh)) else None
    p = start("echo no > %s/h2" % T, kh=kh); wid = window(p); time.sleep(2)
    xdo("type", "--delay", "40", "no"); xdo("key", "Return"); time.sleep(1)
    check("answering no does not connect, the window stays", p.poll() is None and rd("h2") is None and rd(kh) is None)
    close_window(wid)
    check("...and closing it exits with status 1", wait(lambda: p.poll() == 1), p.poll())
    if p.poll() is None: p.kill()

    print("## failing")
    p = subprocess.Popen([BIN, "-i", os.path.join(T, "key"), "-p", "1", "root@127.0.0.1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    wid = window(p); time.sleep(1.5)
    check("a failed connection is shown in the window, which stays", p.poll() is None)
    close_window(wid)
    check("...and closing it exits with status 1", wait(lambda: p.poll() == 1), p.poll())
    if p.poll() is None: p.kill()
finally:
    sshd.kill(); xv.kill(); shutil.rmtree(T, ignore_errors=True)
print("\nSUMMARY: %d/%d passed" % (ok, ok + fail)); sys.exit(1 if fail else 0)
