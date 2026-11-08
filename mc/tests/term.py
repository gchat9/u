"""Run a terminal program under a pseudo-terminal, and read its screen.

The screen is rendered by pyte. A panel is read from the screen: the title is
row 0, the entries are rows 2 to 19, and the cursor is the cyan row.
"""
import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import termios
import time

import pyte

ROWS, COLS = 24, 100
PANEL = COLS // 2                         # the width of one panel
BODY = range(2, 20)                       # the entry rows of a panel
HERE = os.path.dirname(os.path.abspath(__file__))
MC = os.environ.get("MC", os.path.join(HERE, "..", "mc"))
REFERENCE = "/usr/bin/mc"                  # the installed Midnight Commander, if any

UP, DOWN, LEFT, RIGHT = b"\x1bOA", b"\x1bOB", b"\x1bOD", b"\x1bOC"
HOME, END = b"\x1bOH", b"\x1bOF"
PGUP, PGDN = b"\x1b[5~", b"\x1b[6~"
ENTER, ALT_ENTER, ALT_LF = b"\r", b"\x1b\r", b"\x1b\n"
ESC, BS = b"\x1b", b"\x7f"
F3, F5, F6, F8, F10 = b"\x1bOR", b"\x1b[15~", b"\x1b[17~", b"\x1b[19~", b"\x1b[21~"
ALT_C, CTRL_R = b"\x1bc", b"\x12"
INSERT, TAB = b"\x1b[2~", b"\t"

TITLE = re.compile(r"(?:\u250c<\u2500|l<q) (.+?) [\u2500q]+")


class Term:
    """A program on a pseudo-terminal, and its screen."""

    def __init__(self, argv, cwd, env=None, home=None):
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        base = {"TERM": "xterm", "PATH": os.environ["PATH"], "SHELL": "/bin/sh",
                "LANG": "C", "PS1": "$ ", "HOME": home or cwd}
        base.update(env or {})
        self.master = master
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                     cwd=cwd, env=base, start_new_session=True)
        os.close(slave)
        self.base = 0                       # the column the active panel starts at
        self.pump(0.2, 3.0)               # mc is drawn in milliseconds: settle, not wait

    def tab(self):
        """Switch to the other panel, which is the active one from now on."""
        self.send(TAB, .3)
        self.base = PANEL - self.base

    def pump(self, quiet=0.3, limit=5.0):
        """Read the output until it is quiet for QUIET seconds, or LIMIT passes."""
        end = time.time() + limit
        while time.time() < end:
            ready, _, _ = select.select([self.master], [], [], quiet)
            if not ready:
                return
            try:
                data = os.read(self.master, 65536)
                self.sent = getattr(self, "sent", 0) + len(data)
            except OSError:
                return
            if not data:
                return
            try:
                self.stream.feed(data)
            except Exception:           # pyte rejects a few private sequences
                pass

    def send(self, data, wait=0.3):
        os.write(self.master, data)
        self.pump(wait)

    def line(self, y):
        return self.screen.display[y]

    def lines(self):
        return [self.screen.display[y] for y in range(ROWS)]

    def alive(self):
        return self.proc.poll() is None

    def title(self):
        """The path at the top of the active panel."""
        line = self.line(0)[self.base:self.base + PANEL]
        m = TITLE.search(line)
        return m.group(1).strip() if m else line[:40].strip()

    def cursor(self):
        """The body row under the cursor, or None if no row is highlighted."""
        for y in BODY:
            for x in range(self.base + 1, self.base + PANEL - 1):
                if self.screen.buffer[y][x].bg == "cyan":
                    return y - BODY[0]
        return None

    def entries(self):
        """The left panel's entries as (name, size), the name marked: '/' for a
        directory, '@' for a link, ' ' for a file, as mc shows them."""
        out = []
        for y in BODY:
            name = self.line(y)[self.base + 1:self.base + 28].rstrip()
            size = self.line(y)[self.base + 29:self.base + 36].strip()
            if not name and not size:
                break
            out.append((name, size))
        return out

    def cursor_name(self):
        i = self.cursor()
        return None if i is None else self.entries()[i][0]

    def goto(self, index):
        """Move the cursor to entry INDEX, by Down and Up."""
        c = self.cursor()
        while c < index:
            self.send(DOWN, 0.18)
            c += 1
        while c > index:
            self.send(UP, 0.18)
            c -= 1
        assert self.cursor() == index, ("cursor", self.cursor(), index)

    def find(self, name):
        """The index of the entry called NAME (a prefix, as long names are cut)."""
        for i, (shown, _) in enumerate(self.entries()):
            if shown[1:].startswith(name) or shown.startswith(name):
                return i
        return None

    def info_line(self):
        return " ".join(self.line(y) for y in range(20, 23))

    def close(self):
        if self.master is None:
            return
        try:
            self.send(F10, 0.5)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        os.close(self.master)
        self.master = None


def reference_home(home):
    """A home directory for the installed mc, with the layout this suite assumes:
    no menu, key bar, message line or xterm title."""
    os.makedirs(os.path.join(home, ".config", "mc"), exist_ok=True)
    with open(os.path.join(home, ".config", "mc", "ini"), "w") as f:
        f.write("[Layout]\nkeybar_visible=false\nmenubar_visible=false\n"
                "message_visible=false\nxterm_title=false\noutput_lines=0\n")
    return home


def ours(cwd, home=None, env=None):
    return Term([MC], cwd, dict({"LANG": "C"}, **(env or {})), home=home)


def reference(cwd, home, env=None):
    reference_home(home)
    return Term([REFERENCE, "-c"], cwd, dict({"LANG": "C.UTF-8"}, **(env or {})), home=home)
