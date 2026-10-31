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
/* Font size                                                            */
/* ================================================================== */

/* Ctrl+= and Ctrl+- (and the keypad's Ctrl+KP_Add and Ctrl+KP_Subtract) make
 * the font one size larger or smaller: xterm has FONT_SIZES of them, each in
 * its own files (see _font/).  Only without Shift, so that Ctrl+_ (Ctrl+Shift+-)
 * and the other shifted forms reach the program.  Like the paste keys, these
 * keys are not typed to the program.  Define this to 0 to leave them to it; the
 * size is then the start size alone. */
#ifndef XTERM_FONT_KEYS
#define XTERM_FONT_KEYS 1
#endif

/* At start, the font size is the one whose grid is nearest this many lines on
 * the screen (by the screen's pixel height).  Which size that is depends on the
 * screen, as the sizes are fixed steps, so the line count is only close. */
#ifndef XTERM_START_LINES
#define XTERM_START_LINES 25
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

/* ================================================================== */
/* Keyboard (xterm.c)                                                   */
/* ================================================================== */

/* CapsLock shifts letters, as on a real keyboard (and as xterm and st do).  Off
 * by default: the lock state is then ignored and none of the code for it is
 * compiled in, so the default build is no bigger for it.  -DXTERM_CAPSLOCK=1
 * turns it on (about 90 bytes). */
#ifndef XTERM_CAPSLOCK
#define XTERM_CAPSLOCK 0
#endif

/* NumLock and the keypad, as xterm has them: with NumLock on and no Shift, the
 * keypad types its digits, operators and Return; otherwise it is the editing
 * key of its base column (KP_7 is Home with NumLock off), and Shift makes the
 * editing key with a modifier (Shift+KP_1 is ESC [ 1 ; 2 F).  Off by default: a
 * keypad key then always types what it types with NumLock on and no modifier
 * (KP_1 is "1", KP_Home is "7"), and no NumLock or keypad-editing code is compiled
 * in.  -DXTERM_NUMLOCK=1 turns the full behaviour on (about 300 bytes). */
#ifndef XTERM_NUMLOCK
#define XTERM_NUMLOCK 0
#endif
