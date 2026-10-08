import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mouse_lib import *
from PIL import Image, ImageChops
_root = os.path.normpath(os.path.join(HERE, "..", "..", ".."))   # repository root
XTMUX = os.environ.get("XTMUX_BIN", os.path.join(_root, "xtmux", "xtmux")); TMUX = os.environ.get("TMUX_BIN", os.path.join(_root, "tmux", "tmux"))
ESC = b"\x1b"
def xc(sel, *extra):
    try: return subprocess.run(["xclip", "-o", "-selection", sel, *extra], capture_output=True, timeout=5).stdout
    except subprocess.TimeoutExpired: return b"<timeout>"
def own(sel, data):
    subprocess.run(["xclip", "-i", "-selection", sel], input=data, timeout=5); time.sleep(.3)
def drag(c0, r0, c1, r1, mod=None, button="1"):
    if mod: xdo("keydown", mod); time.sleep(.1)
    move(c0, r0); xdo("mousedown", button); time.sleep(.1); move(c1, r1); time.sleep(.1); xdo("mouseup", button); time.sleep(.25)
    if mod: xdo("keyup", mod); time.sleep(.1)
def rclick(c=20, r=8, mod=None):
    if mod: xdo("keydown", mod); time.sleep(.1)
    click(c, r, 3)
    if mod: xdo("keyup", mod); time.sleep(.1)
def boot(res="800x600x24"):
    reset(); sh("pkill -9 xclip"); os.environ["XRES"] = res
    xv = start_xvfb(); xt = start_xtmux(XTMUX); time.sleep(2); return xv, xt
def done(xv, xt, *more):
    for p in (xt,) + more:
        try: p.kill()
        except Exception: pass
    xv.kill(); xv.wait(); sh("pkill -9 -x xtmux; pkill -9 -x tmux; pkill -9 xclip")
def logger(modes, first="ZZ-marker", env=""):
    """clear, print a marker on row 0, then run the byte logger with these DEC modes (row 0 stays 'ZZ-marker')."""
    for f in ("/tmp/mouse.ready", "/tmp/mouse.log"):
        try: os.unlink(f)
        except FileNotFoundError: pass
    xdo("type", "clear; printf '%s\\n'; %s python3 %s %s" % (first, env, os.path.join(HERE, "mouse_log.py"), " ".join(modes))); xdo("key", "Return")
    for _ in range(40):
        if os.path.exists("/tmp/mouse.ready"): break
        time.sleep(.1)
    time.sleep(.5); clear()
def sgr(cb, c, r, f="M"): return ESC + b"[<%d;%d;%d%s" % (cb, c + 1, r + 1, f.encode())
tests = sys.argv[1:] or ["copy","policy","paste","serve","observer"]

if "copy" in tests:
    print("## copying")
    xv, xt = boot()
    xdo("type", "clear; printf 'alpha beta\\ngamma delta\\n'"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 10,1)
    check("drag copies to PRIMARY", xc("primary") == b"alpha beta\ngamma delta", xc("primary"))
    check("...and to CLIPBOARD", xc("clipboard") == b"alpha beta\ngamma delta", xc("clipboard"))
    own("primary", b"sentinel-P"); own("clipboard", b"sentinel-C")
    click(3, 1)
    check("a plain click (no drag) copies nothing", xc("primary") == b"sentinel-P" and xc("clipboard") == b"sentinel-C", (xc("primary"), xc("clipboard")))
    drag(10,1, 0,0)
    check("dragging backwards gives the same text", xc("primary") == b"alpha beta\ngamma delta", xc("primary"))
    xdo("type", "clear; printf 'ab   \\ncd\\n'"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 10,1)
    check("trailing blanks of each line are dropped", xc("primary") == b"ab\ncd", xc("primary"))
    xdo("type", "clear; printf '%0100d\\n' 0"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 38,1)
    check("a wrapped line is copied without a newline in it", xc("primary") == b"0" * 100, xc("primary"))
    xdo("type", "clear; printf '\\303\\251\\344\\270\\255x\\n'"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 3,0)
    check("non-ASCII (é, 中) survives", xc("primary").decode("utf8", "replace") == "é中x", xc("primary"))
    done(xv, xt)
    xv, xt = boot()
    xdo("type", "clear; printf 'row0 text\\n'"); xdo("key","Return"); time.sleep(.8)
    base = Image.open("/tmp/h0.png") if False else None
    shot("/tmp/h_base.png"); crop = lambda p: Image.open(p).convert("RGB").crop((0, 0, 400, 26))
    drag(0,0, 8,0); time.sleep(.3); shot("/tmp/h_sel.png")
    check("selection is highlighted", ImageChops.difference(crop("/tmp/h_base.png"), crop("/tmp/h_sel.png")).getbbox() is not None)
    xdo("type", "x"); time.sleep(.5); shot("/tmp/h_typed.png")
    check("typing removes the highlight (clipboard keeps the text)", ImageChops.difference(crop("/tmp/h_base.png"), crop("/tmp/h_typed.png")).getbbox() is None and xc("primary") == b"row0 text", xc("primary"))
    xdo("key", "BackSpace"); xdo("type", "(sleep 1.5; echo late) &"); xdo("key","Return"); time.sleep(.5)
    drag(0,0, 8,0); time.sleep(.3); shot("/tmp/h_sel2.png")
    time.sleep(2.2); shot("/tmp/h_out.png")
    check("output arriving on screen also removes it", ImageChops.difference(crop("/tmp/h_base.png"), crop("/tmp/h_out.png")).getbbox() is None)
    done(xv, xt)

if "policy" in tests:
    print("## who gets the mouse: the program, or copy and paste")
    xv, xt = boot()
    logger(["1000", "1006"])
    own("primary", b"sentinel-P"); own("clipboard", b"sentinel-C")
    drag(0,0, 8,0)
    check("program wants the mouse: a plain drag goes to it", log() == sgr(0,0,0) + sgr(0,8,0,"m"), log())
    check("...and the clipboard is untouched", xc("primary") == b"sentinel-P" and xc("clipboard") == b"sentinel-C", (xc("primary"), xc("clipboard")))
    clear(); drag(0,0, 8,0, mod="shift")
    check("Shift held: it is a copy, nothing reaches the program", log() == b"", log())
    check("...of what was dragged over (PRIMARY and CLIPBOARD)", xc("primary") == b"ZZ-marker" and xc("clipboard") == b"ZZ-marker", (xc("primary"), xc("clipboard")))
    clear(); rclick(20, 8)
    check("right click goes to the program", log() == sgr(2,20,8) + sgr(2,20,8,"m"), log())
    own("primary", b"PASTED"); clear(); rclick(20, 8, mod="shift")
    check("Shift + right click pastes instead", log() == b"PASTED", log())
    clear(); click(5, 3, 4)
    check("wheel goes to the program", log() == sgr(64,5,3), log())
    clear(); xdo("keydown", "ctrl"); time.sleep(.1); drag(0,0, 8,0); xdo("keyup", "ctrl"); time.sleep(.1)
    check("Ctrl is not the copy modifier (passes through)", log() == sgr(16,0,0) + sgr(16,8,0,"m"), log())
    xdo("key", "ctrl+q"); time.sleep(.5)
    # no mouse requested: everything is ours
    xdo("type", "clear; printf 'ZZ-marker\\n'"); xdo("key","Return"); time.sleep(.8)
    own("primary", b"sentinel-P2"); drag(0,0, 8,0)
    check("program has not asked for the mouse: a plain drag copies", xc("primary") == b"ZZ-marker", xc("primary"))
    done(xv, xt)

if "paste" in tests:
    print("## pasting")
    xv, xt = boot()
    logger([])
    own("primary", b"hello paste"); rclick()
    check("right click pastes PRIMARY into the program", log() == b"hello paste", log())
    clear(); own("primary", b"a\nb\r\nc\x1bd\x07e\tf"); rclick()
    check("LF and CR LF become CR; ESC and BEL are dropped; tab kept", log() == b"a\rb\rcde\tf", log())
    clear(); own("primary", b"\xc3\xa9\xe4\xb8\xad"); rclick()
    check("UTF-8 passes unchanged", log() == b"\xc3\xa9\xe4\xb8\xad", log())
    clear(); sh("pkill -9 xclip"); time.sleep(.5); rclick(); time.sleep(.5)
    check("nothing to paste: nothing sent, still alive", log() == b"" and xt.poll() is None, log())
    clear(); data = b"".join(b"line %05d of a long paste\n" % i for i in range(2300))    # ~ 62 KB
    own("primary", data); rclick(); time.sleep(1.5)
    want = data.replace(b"\n", b"\r")
    check("a 62 KB paste arrives intact (%d bytes)" % len(want), log() == want, "got %d bytes" % len(log()))
    xdo("key", "ctrl+q"); time.sleep(.5)
    logger([], env="MOUSE_LOG_SLOW=0.03"); clear(); data = b"".join(b"slow line %05d\n" % i for i in range(2000))   # 32 KB into a slow reader
    own("primary", data); rclick(); time.sleep(8.0)
    want = data.replace(b"\n", b"\r")
    check("a 32 KB paste into a slow reader arrives intact (%d bytes)" % len(want), log() == want, "got %d bytes" % len(log()))
    xdo("key", "ctrl+q"); time.sleep(.5)
    logger(["2004"]); own("primary", b"two\nlines"); rclick()
    check("bracketed paste: wrapped in ESC[200~ ... ESC[201~", log() == ESC + b"[200~two\rlines" + ESC + b"[201~", log())
    clear(); own("primary", b"x\x1b[201~y"); rclick()
    check("...and the text cannot end the bracket early", log() == ESC + b"[200~x[201~y" + ESC + b"[201~", log())
    clear(); xdo("keydown","shift"); time.sleep(.1); drag(0,0, 8,0); xdo("keyup","shift"); time.sleep(.1)
    drag(0,0, 8,0); clear(); rclick()
    check("pasting what was just copied from the same window", log() == ESC + b"[200~ZZ-marker" + ESC + b"[201~", log())
    done(xv, xt)

if "serve" in tests:
    print("## serving the selection to other clients")
    xv, xt = boot()
    xdo("type", "clear; printf 'ZZ-marker\\n'"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 8,0)
    check("UTF8_STRING", xc("clipboard", "-t", "UTF8_STRING") == b"ZZ-marker", xc("clipboard", "-t", "UTF8_STRING"))
    check("TARGETS lists TARGETS and UTF8_STRING", xc("primary", "-t", "TARGETS").split() == [b"TARGETS", b"UTF8_STRING"], xc("primary", "-t", "TARGETS"))
    check("an unsupported target is refused (nothing returned)", xc("primary", "-t", "STRING") == b"", xc("primary", "-t", "STRING"))
    own("primary", b"someone else's"); time.sleep(.3)
    check("another client taking PRIMARY wins; CLIPBOARD is still ours", xc("primary") == b"someone else's" and xc("clipboard") == b"ZZ-marker", (xc("primary"), xc("clipboard")))
    check("xtmux still alive", xt.poll() is None)
    done(xv, xt)

if "observer" in tests:
    print("## an observer window copies and pastes too")
    xv, xt = boot()
    logger(["2004"])
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.5)          # plain main; the xtmux window is the observer
    drag(0,0, 8,0)
    check("copy from the observer window's frame", xc("primary") == b"ZZ-marker" and xc("clipboard") == b"ZZ-marker", xc("primary"))
    clear(); own("primary", b"y" * 1000); rclick(); time.sleep(1.0)
    check("a 1000-byte paste from the observer reaches the program whole (bracketed, via K packets)", log() == ESC + b"[200~" + b"y" * 1000 + ESC + b"[201~", "got %d bytes" % len(log()))
    clear(); own("primary", b"after"); rclick(); time.sleep(.5)
    check("and the stream is still in sync afterwards", log() == ESC + b"[200~after" + ESC + b"[201~", log())
    p.kill(); done(xv, xt)

if "interop" in tests:
    print("## other X clients (xsel, xclip) and the window's protocol atoms")
    xv, xt = boot()
    wid = sh("xdotool search --name u-tmux 2>/dev/null").split()[0]
    check("WM_PROTOCOLS lists WM_DELETE_WINDOW (so the window manager can ask us to close)",
          "WM_DELETE_WINDOW" in sh("xprop -id %s WM_PROTOCOLS" % wid), sh("xprop -id %s WM_PROTOCOLS" % wid).strip())
    xdo("type", "clear; printf 'test word\\n'"); xdo("key","Return"); time.sleep(.8)
    drag(0,0, 3,0)
    sx = lambda *a: subprocess.run(["xsel", *a], capture_output=True, timeout=5).stdout
    check("xsel -o reads what was copied (PRIMARY)", sx("-o") == b"test", sx("-o"))
    check("xsel -b -o reads it from CLIPBOARD", sx("-b", "-o") == b"test", sx("-b", "-o"))
    check("xclip -o too", xc("primary") == b"test", xc("primary"))
    check("TARGETS is TARGETS + UTF8_STRING", xc("primary", "-t", "TARGETS").split() == [b"TARGETS", b"UTF8_STRING"], xc("primary", "-t", "TARGETS"))
    xdo("key", "ctrl+q") if False else None
    xdo("type", "clear"); xdo("key","Return"); time.sleep(.5)
    logger([])
    subprocess.run("echo test | xsel -i", shell=True, timeout=5); time.sleep(.4); rclick()
    check("echo test | xsel -i, then a right click pastes it", log() == b"test\r" or log() == b"test", log())
    clear(); subprocess.run("echo another-test | xclip -i", shell=True, timeout=5); time.sleep(.4); rclick()
    check("echo another-test | xclip -i, then a right click pastes it", log() == b"another-test\r", log())
    done(xv, xt)

if "keys" in tests:
    print("## paste shortcuts: Shift+Insert, Ctrl+V")
    xv, xt = boot()
    logger([])
    own("primary", b"via shortcut")
    for combo in ("shift+Insert", "ctrl+v", "ctrl+shift+v"):
        clear(); xdo("key", combo); time.sleep(.6)
        check("%s pastes PRIMARY, and sends nothing else" % combo, log() == b"via shortcut", log())
    clear(); xdo("key", "Insert"); time.sleep(.4)
    check("a plain Insert is still the program's key (ESC [ 2 ~)", log() == ESC + b"[2~", log())
    clear(); xdo("key", "ctrl+b"); xdo("key", "ctrl+v") if False else None
    clear(); xdo("key", "alt+v"); time.sleep(.4)
    check("alt+v is not a paste (ESC v reaches the multiplexer/program as before)", log() != b"via shortcut", log())
    xdo("key", "ctrl+q"); time.sleep(.5)
    logger(["1000", "1006"])
    clear(); xdo("key", "shift+Insert"); time.sleep(.6)
    check("with the mouse requested by the program the shortcuts still paste", log() == b"via shortcut", log())
    xdo("key", "ctrl+q"); time.sleep(.5)
    logger(["2004"])
    clear(); xdo("key", "ctrl+v"); time.sleep(.6)
    check("bracketed paste applies to the shortcuts too", log() == ESC + b"[200~via shortcut" + ESC + b"[201~", log())
    clear(); own("primary", b"a\nb"); xdo("key", "shift+Insert"); time.sleep(.6)
    check("same newline handling as a right click", log() == ESC + b"[200~a\rb" + ESC + b"[201~", log())
    clear(); sh("pkill -9 xclip"); time.sleep(.4); xdo("key", "shift+Insert"); time.sleep(.6)
    check("nothing to paste: nothing sent, still alive", log() == b"" and xt.poll() is None, log())
    done(xv, xt)
    xv, xt = boot()
    logger(["2004"])
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.5)
    clear(); own("primary", b"obs shortcut"); xdo("key", "ctrl+v"); time.sleep(.8)
    check("in an observer window too", log() == ESC + b"[200~obs shortcut" + ESC + b"[201~", log())
    p.kill(); done(xv, xt)

print("\nSUMMARY: %d/%d passed" % (sum(results), len(results)))
