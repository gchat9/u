/*
 * xterm_conf.h — compile-time options of the xterm layer: the VT parser
 * (vt.c), and the screen, mouse and clipboard handling that sits between
 * it and the X11 window (xterm.c).  Shared by everything built on them
 * (xtmux, xterm_demo, ...); every option is #ifndef-guarded, so it can be
 * overridden with -D on the compiler command line.  (tmux's own options
 * are in tmux/config.h.)
 */
#pragma once

/* ================================================================== */
/* Mouse (xtmux)                                                        */
/* ================================================================== */

/* xtmux reports the mouse to programs that ask for it (?1000, ?1002,
 * ?1003) using the classic xterm encoding or, if they ask for it
 * (?1006), the SGR one.  Define this to also support the UTF-8 (?1005)
 * and urxvt (?1015) encodings.  Few programs use them, and they cost
 * code; without it a program asking for one of them simply keeps the
 * encoding it had, as on any terminal that lacks it. */
/* #define MOUSE_UTF8_URXVT_ENCODINGS */

/* Copy and paste with the mouse (xtmux): drag with the left button to
 * select text and copy it to PRIMARY and CLIPBOARD, click the right button
 * to paste PRIMARY.  Double-click selects a word, triple-click a line, and
 * dragging after either extends the selection by words or lines; Alt+drag
 * selects a rectangle (with Alt as MOUSE_SELECT_MOD below, that is not
 * available).  When a program has asked for the mouse these go to it
 * instead -- unless this modifier is held, which keeps the mouse for
 * copy and paste: 4 Shift, 8 Alt, 16 Control (add them to need several);
 * 0 leaves the mouse to the program for good. */
#ifndef MOUSE_SELECT_MOD
#define MOUSE_SELECT_MOD 4
#endif

/* ================================================================== */
/* Scrollback                                                           */
/* ================================================================== */

/*
 * Capture lines that scroll off the top into a per-window buffer and
 * inject them into the parent terminal's own scrollback on window switch.
 * Set to 0 to disable entirely (saves SCROLLBACK_BYTES per window).
 */
#ifndef SCROLLBACK_ENABLED
#define SCROLLBACK_ENABLED  1
#endif

/*
 * Size of the per-window scrollback buffer in bytes.
 * Lines are stored as plain UTF-8 with trailing whitespace stripped and
 * a newline appended.  When the buffer is full the oldest line is dropped.
 */
#ifndef SCROLLBACK_BYTES
#define SCROLLBACK_BYTES    32768
#endif
