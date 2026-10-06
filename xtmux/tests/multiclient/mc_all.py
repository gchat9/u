import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mc_lib import *
from PIL import Image, ImageChops
_root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))   # repository root
TMUX = os.environ.get("TMUX_BIN", os.path.join(_root, "tmux", "tmux")); XTMUX = os.environ.get("XTMUX_BIN", os.path.join(_root, "xtmux", "xtmux"))
results = []
def check(name, ok, extra=""):
    results.append(ok); print("  %-4s %s %s" % ("PASS" if ok else "FAIL", name, extra))
def sh(cmd): return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout
def wins(): return sh("xdotool search --name u-tmux 2>/dev/null").split()
def reset():
    sh("pkill -9 -x tmux; pkill -9 -x xtmux; pkill -9 -x openbox; pkill -9 Xvfb; pkill -9 -x sleep; rm -f /run/utmux /tmp/*.mc"); time.sleep(1)
def session_alive(): return os.path.exists("/run/utmux") and bool(sh("pgrep -x xtmux; pgrep -x tmux").split())
def wshot(w, path): sh("xwd -id %s -silent | convert xwd:- %s" % (w, path)); return Image.open(path).convert("RGB")
def differs(a, b): return ImageChops.difference(a, b).getbbox() is not None
def env(res="800x600x24", wm=True):
    reset(); os.environ["XRES"] = res
    xv = start_xvfb(); w = None
    if wm: w = subprocess.Popen(["openbox"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(1)
    return xv, w
def teardown(*ps):
    for p in ps:
        try: p.kill()
        except Exception: pass
    sh("pkill -9 -x tmux; pkill -9 -x xtmux; pkill -9 Xvfb; pkill -9 -x openbox"); time.sleep(0.5)
def typed(w, text):
    sh("xdotool windowactivate --sync %s 2>/dev/null; xdotool windowfocus %s" % (w, w)); time.sleep(0.2)
    xdo("type", text); xdo("key", "Return"); time.sleep(0.8)

tests = sys.argv[1:] or ["keys","shrink","pp","px","xx","detach_pair","detach_lone","close","xloss","obs_close","plainobs_detach","race"]

if "xx" in tests:
    print("## xtmux <-> xtmux sharing (second attach takes over; first becomes observer)")
    xv, wm = env("1920x1080x24")
    a = start_xtmux(XTMUX); time.sleep(2); wa = wins()[0]
    typed(wa, "echo from-first")
    b = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5)
    ids = wins(); wb = [i for i in ids if i != wa][0]
    sh("xdotool windowsize %s 700 400; xdotool windowmove %s 0 0; xdotool windowsize %s 700 400; xdotool windowmove %s 900 0" % (wa, wa, wb, wb)); time.sleep(1.5)
    check("both windows up, both processes alive", len(ids) == 2 and a.poll() is None and b.poll() is None)
    before = wshot(wa, "/tmp/xx_a0.png")
    typed(wb, "echo typed-in-main > /tmp/xx1.mc")
    check("typing in main (2nd) window ran in the shared shell", os.path.exists("/tmp/xx1.mc"))
    check("first (observer) window mirrored the change", differs(before, wshot(wa, "/tmp/xx_a1.png")))
    before = wshot(wb, "/tmp/xx_b0.png")
    typed(wa, "echo typed-in-observer > /tmp/xx2.mc")
    check("typing in observer (1st) window ran in the shared shell", os.path.exists("/tmp/xx2.mc"))
    check("main window mirrored the change", differs(before, wshot(wb, "/tmp/xx_b1.png")))
    check("observer window leaked nothing to its launching terminal", a.tty_text() == "", repr(a.tty_text()[:40]))
    teardown(a, b, xv, wm)

if "detach_pair" in tests:
    print("## detach/promotion with two xtmux windows (main detaches -> observer promoted)")
    xv, wm = env()
    a = start_xtmux(XTMUX); time.sleep(2); wa = wins()[0]
    b = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5)
    wb = [i for i in wins() if i != wa][0]
    typed(wb, "")  # focus main
    xdo("key", "ctrl+b"); xdo("key", "d"); time.sleep(2.5)
    check("main's window disappeared on detach", wb not in wins())
    check("observer's window still there and its process alive", wa in wins() and a.poll() is None)
    typed(wa, "echo promoted > /tmp/dp1.mc")
    check("promoted window drives the session", os.path.exists("/tmp/dp1.mc"))
    shot("/tmp/dp_after.png")
    teardown(a, b, xv, wm)

if "detach_lone" in tests:
    print("## lone xtmux detaches -> daemon; window must vanish; others can attach")
    xv, wm = env()
    a = start_xtmux(XTMUX); time.sleep(2); wa = wins()[0]
    typed(wa, "sleep 777 &")
    xdo("key", "ctrl+b"); xdo("key", "d"); time.sleep(1.5)
    check("window gone after detach", not wins())
    check("launching process exited, daemon alive, shell job survives", a.poll() is not None and session_alive() and "sleep" in sh("pgrep -a sleep"))
    c = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5)
    check("xtmux can re-attach to the daemon", len(wins()) == 1 and c.poll() is None)
    typed(wins()[0], "echo back > /tmp/dl1.mc")
    check("re-attached session works", os.path.exists("/tmp/dl1.mc"))
    teardown(a, c, xv, wm)

if "close" in tests:
    print("## window-manager close")
    xv, wm = env()
    a = start_xtmux(XTMUX); time.sleep(2); wa = wins()[0]; typed(wa, "sleep 778 &")
    sh("wmctrl -c u-tmux"); time.sleep(1.5)
    check("lone: window closed, process detached (daemon, job alive)", not wins() and session_alive() and "sleep" in sh("pgrep -a sleep"))
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.0)
    check("lone: plain can attach to it afterwards", "root@" in p.text())
    p.kill(); teardown(a, xv, wm)
    xv, wm = env()
    p = PlainTmux(TMUX); p.pump(1.0)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); p.pump(1.0)
    sh("wmctrl -c u-tmux"); time.sleep(2.5); p.pump(1.5)
    p.send(b"echo promoted-plain > /tmp/cl1.mc\n"); p.pump(1.0)
    check("with plain observer: closing xtmux promotes plain, which keeps working", p.alive() and os.path.exists("/tmp/cl1.mc"))
    p.kill(); teardown(x, xv, wm)

if "xloss" in tests:
    print("## X connection lost")
    xv, wm = env(wm=False)
    a = start_xtmux(XTMUX); time.sleep(2); typed(wins()[0], "sleep 779 &")
    xv.kill(); xv.wait(); time.sleep(2.0)
    check("lone: xtmux detached (launcher exited), session + job survive", a.poll() is not None and session_alive() and "sleep" in sh("pgrep -a sleep"), a.tty_text()[-60:])
    xv = start_xvfb()
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.0)
    check("lone: a client can attach to what survived", "root@" in p.text())
    p.kill(); teardown(a, xv)
    xv, wm = env(wm=False)
    p = PlainTmux(TMUX); p.pump(1.0)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); p.pump(1.0)
    xv.kill(); xv.wait(); time.sleep(2.5); p.pump(1.5)
    p.send(b"echo after-xloss > /tmp/xl1.mc\n"); p.pump(1.0)
    check("with plain observer: X loss promotes plain, session continues", p.alive() and os.path.exists("/tmp/xl1.mc"))
    p.kill(); teardown(x, xv)

if "obs_close" in tests:
    print("## closing the *observer* xtmux window")
    xv, wm = env()
    p = PlainTmux(TMUX); p.pump(1.0)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5)       # x main, plain observer
    p2 = PlainTmux(TMUX, ["attach"]); p2.pump(2.5)            # p2 main, xtmux observer
    check("xtmux demoted (still alive)", x.poll() is None)
    sh("wmctrl -c u-tmux"); time.sleep(2.0); p2.pump(1.0)
    p2.send(b"echo main-continues > /tmp/oc1.mc\n"); p2.pump(1.0)
    check("closing observer window ends only that client; main continues", x.poll() is not None and p2.alive() and os.path.exists("/tmp/oc1.mc"))
    p.kill(); p2.kill(); teardown(x, xv, wm)

if "plainobs_detach" in tests:
    print("## plain observer detaches while xtmux is main")
    xv, wm = env()
    p = PlainTmux(TMUX); p.pump(1.0)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); p.pump(1.0)
    p.send(b"\x02d"); p.pump(1.5)
    typed(wins()[0], "echo xmain > /tmp/pd1.mc")
    check("xtmux main unaffected by observer's detach", x.poll() is None and os.path.exists("/tmp/pd1.mc"))
    p.kill(); teardown(x, xv, wm)

if "race" in tests:
    print("## attach-to-daemon race (was 2/6 failures even plain->plain)")
    oks = []
    xv, wm = env(wm=False)
    for i in range(8):
        sh("rm -f /run/utmux; pkill -9 -x tmux; pkill -9 -x xtmux"); time.sleep(0.5)
        if i % 2 == 0:
            a = PlainTmux(TMUX); a.pump(0.7); a.send(b"\x02d"); a.pump(0.7); a.kill_ = a
        else:
            a = start_xtmux(XTMUX); time.sleep(1.5); xdo("key","ctrl+b"); xdo("key","d"); time.sleep(1.0)
        b = PlainTmux(TMUX, ["attach"]); b.pump(1.3); oks.append("root@" in b.text()); b.kill()
    check("8/8 attaches after detach succeed (plain and xtmux daemons)", all(oks), str(oks))
    teardown(xv)


if "pp" in tests:
    print("## plain <-> plain (regression: the shared observer loop was rewritten)")
    xv, wm = env(wm=False)
    a = PlainTmux(TMUX); a.pump(1.0); a.send(b"echo A1\n"); a.pump(0.6)
    b = PlainTmux(TMUX, ["attach"]); b.pump(2.0)
    check("attach takes over; new main shows session", "A1" in b.text())
    b.send(b"echo typed-in-new-main\n"); b.pump(1.0); a.pump(1.0)
    check("old main (now observer) mirrors", "typed-in-new-main" in a.text())
    a.send(b"echo typed-in-observer\n"); a.pump(1.0); b.pump(1.0)
    check("observer input reaches session and new main sees it", "typed-in-observer" in b.text())
    b.send(b"\x02d"); b.pump(2.5); a.pump(1.5)
    a.send(b"echo promoted > /tmp/pp1.mc\n"); a.pump(1.0)
    check("main detaches -> observer promoted and works", a.alive() and os.path.exists("/tmp/pp1.mc"))
    c = PlainTmux(TMUX, ["attach"]); c.pump(2.0); a.pump(1.0)
    c.send(b"echo third > /tmp/pp2.mc\n"); c.pump(1.0)
    check("a third client can join afterwards", os.path.exists("/tmp/pp2.mc"))
    for p in (a, b, c): p.kill()
    teardown(xv)

if "px" in tests:
    print("## the reported scenario: plain starts, xtmux attaches")
    xv, wm = env("1920x1080x24")
    p = PlainTmux(TMUX); p.pump(1.0); p.send(b"echo from-plain\n"); p.pump(0.6)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); p.pump(1.0)
    w = wins()[0]
    check("xtmux shows the session", differs(wshot(w, "/tmp/px0.png"), Image.new("RGB", Image.open("/tmp/px0.png").size, (13,17,23))))
    typed(w, "echo typed-in-xtmux > /tmp/px1.mc"); p.pump(1.0)
    check("typing in xtmux works and plain mirrors it", os.path.exists("/tmp/px1.mc") and "typed-in-xtmux" in p.text())
    p.send(b"echo typed-in-plain > /tmp/px2.mc\n"); p.pump(1.0)
    check("typing in the plain observer works", os.path.exists("/tmp/px2.mc"))
    check("both alive", x.poll() is None and p.alive())
    xdo("key","ctrl+b"); xdo("key","d"); time.sleep(2.5); p.pump(1.5)
    p.send(b"echo p-promoted > /tmp/px3.mc\n"); p.pump(1.0)
    check("xtmux detaches: window gone, plain resumes as main", not wins() and os.path.exists("/tmp/px3.mc"))
    p.kill(); teardown(x, xv, wm)

if "keys" in tests:
    print("## special keys typed in an observer arrive intact (was: ESC [ A became NUL ESC A)")
    def od_last(p): 
        r = [l.strip() for l in p.text().split("\n") if l.strip().startswith("0000000")]; return r[-1] if r else ""
    want = "0000000 033   [   A 033   [   B  \\n"
    xv, wm = env(wm=False)
    # xtmux as observer (plain attaches last and becomes main)
    x = start_xtmux(XTMUX); time.sleep(2)
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.0)
    xdo("type", "od -c"); xdo("key", "Return"); time.sleep(0.4)
    xdo("key", "Up"); xdo("key", "Down"); xdo("key", "Return"); xdo("key", "ctrl+d"); time.sleep(1.2); p.pump(1.5)
    check("xtmux observer: Up/Down reach the program as ESC [ A / ESC [ B", od_last(p) == want, od_last(p))
    p.kill(); teardown(x, xv)
    # plain as observer
    sh("rm -f /run/utmux"); a = PlainTmux(TMUX); a.pump(1.0); b = PlainTmux(TMUX, ["attach"]); b.pump(2.0)
    a.send(b"od -c\n"); a.pump(0.6); a.send(b"\x1b[A\x1b[B\n\x04"); a.pump(1.5); b.pump(1.0)
    check("plain observer: same", od_last(b) == want, od_last(b))
    a.kill(); b.kill(); teardown(xv)

if "shrink" in tests:
    print("## attaching from a smaller screen keeps short output visible")
    def nb(p): return [l for l in p.text().split("\n") if l.strip()]
    sh("rm -f /run/utmux; pkill -9 -x tmux"); time.sleep(0.7)
    a = PlainTmux(TMUX, rows=40, cols=100); a.pump(1.0); a.send(b"pwd; echo hello-short\n"); a.pump(0.8)
    b = PlainTmux(TMUX, ["attach"], rows=24, cols=80); b.pump(2.0)
    check("short output still on screen after shrinking 40 -> 24 rows", any("hello-short" in l for l in nb(b)) and any("pwd" in l for l in nb(b)))
    a.kill(); b.kill(); sh("rm -f /run/utmux; pkill -9 -x tmux"); time.sleep(0.7)
    a = PlainTmux(TMUX, rows=40, cols=100); a.pump(1.0); a.send(b"seq 1 60\n"); a.pump(1.0)
    b = PlainTmux(TMUX, ["attach"], rows=24, cols=80); b.pump(2.0)
    L = [l.strip() for l in nb(b)]
    check("full screen: bottom stays anchored (prompt visible, top cropped)", any(l.startswith("root@") for l in L) and "60" in L and "1" not in L)
    b.send(b"\x02["); b.pump(0.6)
    check("...and the cropped lines are in scrollback", any("SCROLLBACK" in l for l in nb(b)))
    a.kill(); b.kill(); sh("rm -f /run/utmux; pkill -9 -x tmux"); time.sleep(0.7)
    xv, wm = env("800x600x24", wm=False)
    a = PlainTmux(TMUX, rows=40, cols=100); a.pump(1.0); a.send(b"echo hello-xshort\n"); a.pump(0.8)
    x = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); a.pump(1.0)
    check("xtmux (smaller grid) attaching also keeps it visible (seen via the plain observer's mirror)", any("hello-xshort" in l for l in nb(a)))
    a.kill(); teardown(x, xv)

print("\nSUMMARY: %d/%d passed" % (sum(results), len(results)))
