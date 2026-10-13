#!/usr/bin/env python3
# usage: prompt_resize.py XTMUX [MIN_COLS]
# bash repaints its input after a window resize.  Fill the screen (and some scrollback), type a
# long command at a long prompt (not run), then step the width one column at a time down to
# MIN_COLS (default 12, narrower than the prompt, so it wraps) and back to the start.  After
# each step the live screen is copied with the mouse (its cells, not its pixels): the prompt
# must appear once (or not at all when the input is taller than the screen), and its first
# letter must not run on across a row.
# Needs Xvfb, xdotool, xclip.  Prints one line per step that is wrong, and a summary.
# SHOW=61,60,... (env) instead shows the copied screen text after each of those widths.
import sys, os, time, subprocess, re
XT = sys.argv[1]
MIN = int(sys.argv[2]) if len(sys.argv) > 2 else 12
CW, CH = 13, 26
PROMPT = "QZ-prompt-with-a-rather-long-path-/home/claude/u/xtmux/tests$ "
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True).stdout
subprocess.run("pkill -x xtmux; pkill Xvfb", shell=True); time.sleep(0.5)
subprocess.Popen("Xvfb :2 -screen 0 800x600x24 -nolisten tcp", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1); os.environ["DISPLAY"] = ":2"
subprocess.Popen("sleep 300 | %s 2>/tmp/prompt_resize.err" % XT, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)
WID = sh("xdotool search --name u-tmux | head -1").strip()
W0, H = 800, 600
def xdo(*a): subprocess.run(["xdotool", *a])
def resize(cols):
    CUR[0] = cols
    subprocess.run(["xdotool", "windowsize", WID, str(cols * CW), str(H)]); time.sleep(0.4)
CUR = [61]
def live_copy():
    # cell centres, as the clipboard tests place them: the top-left cell to the bottom-right one
    cols = CUR[0]
    xdo("mousemove", str(3 + 6), str(1 + 13)); time.sleep(0.05)
    xdo("mousedown", "1"); time.sleep(0.05)
    xdo("mousemove", str(3 + (cols - 1) * CW + 6), str(1 + 21 * 26 + 13)); time.sleep(0.05)
    xdo("mouseup", "1"); time.sleep(0.3)
    return subprocess.run(["xclip", "-o", "-selection", "primary"], capture_output=True).stdout.decode("utf8", "replace")
INPUT = len(PROMPT) + len("echo " + " ".join("word%02d" % i for i in range(40)))
SCREEN_ROWS = 22
def check(label, screen, bad, cols):
    count = screen.count("QZ-prompt")
    run = re.search(r"Q{3,}|Z{3,}", screen)
    problems = []
    # the prompt is on the screen once; only an input taller than the screen may push it off the top
    tall = -(-INPUT // cols) > SCREEN_ROWS
    if count > 1 or (count == 0 and not tall): problems.append("prompt x%d" % count)
    if run: problems.append("letter run %r" % run.group(0)[:12])
    if problems:
        bad.append(label); print("  %-16s %s" % (label, "; ".join(problems)))
    return not problems
xdo("type", "export PS1='%s'; clear; seq 1 60" % PROMPT); xdo("key", "Return"); time.sleep(1)
xdo("type", "echo " + " ".join("word%02d" % i for i in range(40)))   # typed, not run
time.sleep(1)
start = 800 // CW                # 61 columns
steps = list(range(start, MIN - 1, -1)) + list(range(MIN, start + 1))
bad = []; ok = 0
if os.environ.get("SHOW"):
    for cols in [int(x) for x in os.environ["SHOW"].split(",")]:
        resize(cols); time.sleep(0.6)
        c = live_copy()
        sh("xwd -root -silent | convert xwd:- /tmp/pr_%d.png" % cols)
        print("== %d columns: %d prompts" % (cols, c.count("QZ-prompt")))
        for line in c.split("\n"): print("   |" + line[:70])
    sys.exit(0)
for cols in steps:
    resize(cols); time.sleep(0.25)
    if check("%d columns" % cols, live_copy(), bad, cols): ok += 1
print("%d/%d steps clean; widths %d..%d" % (ok, len(steps), MIN, start))
subprocess.run("pkill -x xtmux; pkill Xvfb", shell=True)
sys.exit(0 if not bad else 1)
