#!/usr/bin/env python3
import os
import sys
import tty
import termios

## DEC private mode escape sequences
# Mode Meaning
# 1000 Report button press/release
# 1002 Report movement while a button is pressed
# 1003 Report all mouse movement
# 1006 Use SGR-style mouse encoding

ENABLE = "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h"
DISABLE = "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"

if not (os.isatty(0) and os.isatty(1)):
    sys.exit("Run this with stdin and stdout attached to a terminal.")

fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
last_len = 0
pressed = []


def show(x, y, event, button, mods):
    global last_len

    mods_s = ",".join(mods) if mods else "-"
    pressed_s = ",".join(pressed) if pressed else "-"

    line = (
        f"x={x:4} y={y:4} "
        f"{event:<7} {button:<8} "
        f"mods={mods_s:<12} pressed={pressed_s:<9}"
    )

    try:
        max_len = max(1, os.get_terminal_size(1).columns - 1)
    except OSError:
        max_len = 79

    if last_len > max_len:
        last_len = max_len

    if len(line) > max_len:
        line = line[:max_len]

    if len(line) < last_len:
        line += " " * (last_len - len(line))

    last_len = len(line)

    sys.stdout.write(line + "\r")
    sys.stdout.flush()


def mouse_event(code, x, y, final):
    mods = []
    if code & 4:
        mods.append("shift")
    if code & 8:
        mods.append("alt")
    if code & 16:
        mods.append("ctrl")

    motion = bool(code & 32)
    low = code & 3

    if code & 64:
        event = "wheel"
        button = ["up", "down", "left", "right"][low]

    elif final == "m":
        event = "release"
        button = {0: "left", 1: "middle", 2: "right"}.get(low, "?")

    elif low == 3:
        if motion:
            event = "move"
            button = "-"
        else:
            event = "release"
            button = "?"

    else:
        button = {0: "left", 1: "middle", 2: "right"}[low]
        event = "drag" if motion else "press"

    if event == "press" and button in ("left", "middle", "right"):
        if button not in pressed:
            pressed.append(button)

    elif event == "release":
        if button in pressed:
            pressed.remove(button)
        elif button == "?" and pressed:
            pressed.pop()

    show(x, y, event, button, mods)


try:
    tty.setcbreak(fd)

    sys.stdout.write(ENABLE)
    sys.stdout.write("\r")
    sys.stdout.flush()

    buf = bytearray()

    while True:
        ch = sys.stdin.buffer.read(1)
        if not ch:
            break

        # Quit on bare q or Ctrl-C, but not on bytes inside a mouse report.
        if not buf and ch in (b"q", b"\x03"):
            break

        buf.extend(ch)

        if buf[0] != 0x1b:
            buf.clear()
            continue

        if len(buf) == 1:
            continue

        if buf[1] == ord("["):
            if len(buf) == 2:
                continue

            # Legacy mouse report:
            #   ESC [ M Cb Cx Cy
            # where Cb, Cx, Cy are each value + 32.
            if buf[2] == ord("M"):
                if len(buf) >= 6:
                    mouse_event(buf[3] - 32, buf[4] - 32, buf[5] - 32, None)
                    buf.clear()
                elif len(buf) > 64:
                    buf.clear()
                continue

            # SGR mouse report:
            #   ESC [ < Pb ; Px ; Py M/m
            if buf[2] == ord("<"):
                if buf[-1] in (ord("M"), ord("m")):
                    try:
                        parts = buf[3:-1].decode("ascii").split(";")
                        if len(parts) >= 3:
                            b, x, y = map(int, parts[:3])
                            mouse_event(b, x, y, chr(buf[-1]))
                    except Exception:
                        pass
                    buf.clear()
                elif len(buf) > 64:
                    buf.clear()
                continue

            # Some other CSI sequence: discard it when final byte arrives.
            if 0x40 <= buf[-1] <= 0x7e:
                buf.clear()
            elif len(buf) > 64:
                buf.clear()
            continue

        # Unknown non-CSI escape sequence.
        buf.clear()

except KeyboardInterrupt:
    pass

finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
    sys.stdout.write(DISABLE + "\r\n")
    sys.stdout.flush()
