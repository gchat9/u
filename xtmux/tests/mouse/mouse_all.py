import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mouse_lib import *
_root = os.path.normpath(os.path.join(HERE, "..", "..", ".."))   # repository root
XTMUX = os.environ.get("XTMUX_BIN", os.path.join(_root, "xtmux", "xtmux")); TMUX = os.environ.get("TMUX_BIN", os.path.join(_root, "tmux", "tmux"))
ESC = b"\x1b"
def sgr(cb, c, r, final="M"): return ESC + b"[<%d;%d;%d%s" % (cb, c + 1, r + 1, final.encode())
def classic(cb, c, r): return ESC + b"[M" + bytes([cb + 32, c + 33, r + 33])
def boot(res="800x600x24"):
    reset(); os.environ["XRES"] = res
    xv = start_xvfb(); xt = start_xtmux(XTMUX); time.sleep(2); return xv, xt
def done(xv, xt): 
    xt.kill(); xv.kill(); xv.wait(); sh("pkill -9 -x xtmux; pkill -9 -x tmux")
tests = sys.argv[1:] or ["enc","drag","mods","toggle","edges","windows","handoff","observer"]

if "enc" in tests:
    print("## encodings")
    xv, xt = boot("1920x1080x24")
    check("no mode requested: pointer events are not delivered", (click(5,3), log())[1] == b"")
    start_logger(None, ["1000"]);            click(5,3)
    check("1000 classic: left press+release", log() == classic(0,5,3) + classic(3,5,3), log().hex(" "))
    clear(); click(5,3,3)
    check("1000 classic: right button", log() == classic(2,5,3) + classic(3,5,3), log().hex(" "))
    clear(); click(5,3,4)
    check("1000 classic: wheel up is a press only", log() == classic(64,5,3), log().hex(" "))
    clear(); click(5,3,5)
    check("1000 classic: wheel down", log() == classic(65,5,3), log().hex(" "))
    clear(); click(5,3,2)
    check("1000 classic: middle button", log() == classic(1,5,3) + classic(3,5,3), log().hex(" "))
    stop_logger()
    start_logger(None, ["1000", "1006"]);    click(5,3)
    check("1006 SGR: press M, release m", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    clear(); click(7,10,3)
    check("1006 SGR: right release keeps its button", log() == sgr(2,7,10) + sgr(2,7,10,"m"), log())
    clear(); click(5,3,4)
    check("1006 SGR: wheel", log() == sgr(64,5,3), log())
    clear(); click(140,30)
    check("1006 SGR: large coordinates", log() == sgr(0,140,30) + sgr(0,140,30,"m"), log())
    stop_logger()
    start_logger(None, ["1000", "1015"]);    click(5,3)
    check("1015 urxvt: decimal, button+32", log() == ESC + b"[32;6;4M" + ESC + b"[35;6;4M", log())
    stop_logger()
    start_logger(None, ["1000", "1005"]);    click(100,3)
    check("1005 UTF-8: column 100 takes two bytes", log() == ESC + b"[M" + b"\x20" + "\u0085".encode() + b"\x24" + ESC + b"[M" + b"\x23" + "\u0085".encode() + b"\x24", log().hex(" "))
    stop_logger()
    start_logger(None, ["1000", "1006", "1005"]); click(5,3)   # last set wins
    check("two encodings requested: the later (?1005 after ?1006) is used", log().startswith(ESC + b"[M"), log().hex(" "))
    stop_logger(); done(xv, xt)

if "drag" in tests:
    print("## motion modes")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    move(5,3); xdo("mousedown","1"); time.sleep(.1); move(7,3); move(7,3); xdo("mouseup","1"); time.sleep(.15)
    check("1000: drag gives press and release, no motion", log() == sgr(0,5,3) + sgr(0,7,3,"m"), log())
    stop_logger()
    start_logger(None, ["1002", "1006"])
    move(5,3); xdo("mousedown","1"); time.sleep(.1); move(7,3); move(7,3); xdo("mouseup","1"); time.sleep(.15)
    check("1002: drag adds one motion per new cell", log() == sgr(0,5,3) + sgr(32,7,3) + sgr(0,7,3,"m"), log())
    clear(); move(9,3); move(10,3)
    check("1002: movement with no button held is not reported", log() == b"", log())
    stop_logger()
    start_logger(None, ["1003", "1006"])
    move(9,5); move(10,5); move(10,5); move(11,5)
    check("1003: every new cell reported as no-button motion (35), none for the same cell", log() == sgr(35,9,5) + sgr(35,10,5) + sgr(35,11,5), log())
    clear(); xdo("mousedown","3"); time.sleep(.1); move(12,5); xdo("mouseup","3"); time.sleep(.15)
    check("1003: right-button drag reports button 2 + motion", log() == sgr(2,11,5) + sgr(34,12,5) + sgr(2,12,5,"m"), log())
    stop_logger(); done(xv, xt)

if "mods" in tests:
    print("## modifiers")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    for key, bit in (("ctrl", 16), ("shift", 4), ("alt", 8)):
        clear(); xdo("keydown", key); time.sleep(.1); click(5,3); xdo("keyup", key); time.sleep(.15)
        check("%s+click sets modifier bit %d (and nothing else leaks)" % (key, bit), log() == sgr(bit,5,3) + sgr(bit,5,3,"m"), log())
    clear(); xdo("keydown","ctrl"); xdo("keydown","shift"); time.sleep(.1); click(5,3); xdo("keyup","shift"); xdo("keyup","ctrl"); time.sleep(.15)
    check("ctrl+shift+click combines bits", log() == sgr(20,5,3) + sgr(20,5,3,"m"), log())
    stop_logger(); done(xv, xt)

if "toggle" in tests:
    print("## turning the mode off and on")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    click(5,3); check("enabled: delivered", log() != b"")
    xdo("type", "d"); time.sleep(.5); clear(); click(5,3)
    check("?1000l: pointer events stop", log() == b"", log())
    xdo("type", "e"); time.sleep(.5); clear(); click(5,3)
    check("?1000h again: they resume", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    stop_logger(); done(xv, xt)

if "edges" in tests:
    print("## edges: status row, clamping, classic range, scrollback")
    xv, xt = boot()
    xdo("type", "seq 1 100"); xdo("key", "Return"); time.sleep(.8)
    start_logger(None, ["1000", "1006"])
    click(5, 22)
    check("status row (row 22 of 23) is not the program's area", log() == b"", log())
    clear(); click(5, 21)
    check("last program row (21) is", log() == sgr(0,5,21) + sgr(0,5,21,"m"), log())
    clear(); move(5,3); xdo("mousedown","1"); time.sleep(.1); 
    xdo("mousemove", "799", "100"); time.sleep(.1); xdo("mouseup","1"); time.sleep(.15)
    check("a drag released outside the window is clamped to the last column (61), same row", log() == sgr(0,5,3) + sgr(0,60,3,"m"), log())
    clear()
    xdo("key", "ctrl+b"); xdo("key", "bracketleft"); time.sleep(.6); click(5,3)
    check("scrollback viewer open: nothing is forwarded", log() == b"", log())
    xdo("key", "Escape"); time.sleep(.6); click(5,3)
    check("viewer closed: forwarded again", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    stop_logger(); done(xv, xt)
    xv, xt = boot("4000x1000x24")
    start_logger(None, ["1000"])
    click(250, 3); click(10, 3)
    check("classic encoding drops positions it cannot represent (col 250), still sends col 10", log() == classic(0,10,3) + classic(3,10,3), log().hex(" "))
    stop_logger(); done(xv, xt)

if "windows" in tests:
    print("## per-window modes")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    xdo("key", "ctrl+b"); xdo("key", "2"); time.sleep(.8); clear(); click(5,3)
    check("window 2 (a plain shell) gets nothing", log() == b"", log())
    xdo("key", "ctrl+b"); xdo("key", "1"); time.sleep(.8); click(5,3)
    check("back in window 1 the program gets it", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    stop_logger(); done(xv, xt)

if "handoff" in tests:
    print("## the request survives detaching and re-attaching")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    xdo("key", "ctrl+b"); xdo("key", "d"); time.sleep(2.0)
    xt2 = start_xtmux(XTMUX, ["attach"]); time.sleep(2.5); clear(); click(5,3)
    check("new xtmux instance forwards without the program re-enabling it", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    stop_logger(); done(xv, xt2); xt.kill()

if "observer" in tests:
    print("## xtmux window as an observer")
    xv, xt = boot()
    start_logger(None, ["1000", "1006"])
    p = PlainTmux(TMUX, ["attach"]); p.pump(2.5)          # plain becomes main; xtmux the observer
    clear(); click(5,3)
    check("click in the observer window reaches the program", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    clear(); click(7,10,4)
    check("wheel too", log() == sgr(64,7,10), log())
    p.send(b"d"); p.pump(0.8); time.sleep(.5); clear(); click(5,3)
    check("program turns the mode off -> observer stops forwarding", log() == b"", log())
    p.send(b"e"); p.pump(0.8); time.sleep(.5); clear(); click(5,3)
    check("...and on again", log() == sgr(0,5,3) + sgr(0,5,3,"m"), log())
    p.kill(); done(xv, xt)

print("\nSUMMARY: %d/%d passed" % (sum(results), len(results)))
