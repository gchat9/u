/*
 * xterm_conf.h — compile-time options of the xterm layer: the VT parser
 * (vt.c), and the screen, mouse and clipboard handling that sits between
 * it and the X11 window (xterm.c).  Shared by everything built on them
 * (xtmux, xterm_demo, ..., and the font generator _font/mkfont.c).  (tmux's
 * own options are in tmux/config.h.)
 *
 * This is the one place to configure them: edit the values below, and see
 * what is not default with "git diff".  After a change run make (the
 * programs) and, if it is a keyboard language, make -C _font install (the
 * fonts, which take the glyphs of the languages from here too).
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
#define MOUSE_SELECT_MOD 4

/* ================================================================== */
/* Font size                                                            */
/* ================================================================== */

/* Ctrl+= and Ctrl+- (and the keypad's Ctrl+KP_Add and Ctrl+KP_Subtract) make
 * the font one size larger or smaller: xterm has FONT_SIZES of them, each in
 * its own files (see _font/).  Only without Shift, so that Ctrl+_ (Ctrl+Shift+-)
 * and the other shifted forms reach the program.  Like the paste keys, these
 * keys are not typed to the program.  Define this to 0 to leave them to it; the
 * size is then the start size alone. */
#define XTERM_FONT_KEYS 1

/* At start, the font size is the one whose grid is nearest this many lines on
 * the screen (by the screen's pixel height).  Which size that is depends on the
 * screen, as the sizes are fixed steps, so the line count is only close. */
#define XTERM_START_LINES 25

/* ================================================================== */
/* Scrollback                                                           */
/* ================================================================== */

/*
 * Capture lines that scroll off the top into a per-window buffer and
 * inject them into the parent terminal's own scrollback on window switch.
 * Set to 0 to disable entirely (saves SCROLLBACK_BYTES per window).
 */
#define SCROLLBACK_ENABLED  1

/*
 * Size of the per-window scrollback buffer in bytes.
 * Lines are stored as plain UTF-8 with trailing whitespace stripped and
 * a newline appended.  When the buffer is full the oldest line is dropped.
 */
#define SCROLLBACK_BYTES    32768

/* ================================================================== */
/* Cell grids                                                           */
/* ================================================================== */

/*
 * A window's screen is kept as 2 bytes a cell for as long as everything on it
 * is something the font draws, in not too many different styles, and as 8
 * bytes a cell (what any Unicode character and the 256 colours need) from the
 * first moment it is not.  It is decided per grid (the alternate screen has its
 * own), and the full-size grid goes back to the small one when the screen is
 * cleared or resized and everything fits again.
 *
 * A small cell is a glyph number (9 bits) and a style number (7 bits).  The
 * glyph numbers are those of _font/charset.h: ASCII and Latin-1, then the
 * characters the font has besides them, 511 glyphs at most.  The glyph number
 * 511 stands for a double-width character: the cell after it holds all 16 bits
 * of the glyph's index among the double-width glyphs (256) and the emoji (255).
 * The styles are the
 * fg/bg/attribute combinations on the grid, up to 128 of them: the programs we
 * know use a dozen or two, and the ones that are no longer on the screen are
 * dropped when the table fills up.  A character the font does not have (it is
 * drawn as a box), or the 129th style in use, makes the grid full-size.
 * Nothing is ever lost or approximated.  (Overwriting, erasing or shifting half
 * of a double-width character blanks the other half, in either format.)
 *
 * What it saves is 75% of a grid (66x24: 12K -> 3K) per window that is in the
 * small format, for about 3K of code.  Set to 0 to always use the full-size
 * cells.
 */
#define VT_CELL_COMPACT     1

/* ================================================================== */
/* Keyboard (xterm.c)                                                   */
/* ================================================================== */

/* CapsLock shifts letters, as on a real keyboard (and as xterm and st do).  Off
 * by default: the lock state is then ignored and none of the code for it is
 * compiled in, so the default build is no bigger for it.  1 turns it on (about
 * 90 bytes). */
#define XTERM_CAPSLOCK 0

/* NumLock and the keypad, as xterm has them: with NumLock on and no Shift, the
 * keypad types its digits, operators and Return; otherwise it is the editing
 * key of its base column (KP_7 is Home with NumLock off), and Shift makes the
 * editing key with a modifier (Shift+KP_1 is ESC [ 1 ; 2 F).  Off by default: a
 * keypad key then always types what it types with NumLock on and no modifier
 * (KP_1 is "1", KP_Home is "7"), and no NumLock or keypad-editing code is compiled
 * in.  1 turns the full behaviour on (about 300 bytes). */
#define XTERM_NUMLOCK 0

/* Keyboard layout groups held: the layouts of "setxkbmap -layout us,gr" (each
 * takes 2 KB).  1, 2 or 4.  A key from a group beyond these is typed from the
 * group its number wraps to. */
#define XTERM_GROUPS 2

/* The next three options each add a script: the keys that type it (here) and
 * its glyphs (_font/charset.h, 49 to 95 of the font's 511 narrow slots, and in
 * the programs' small cells, see VT_CELL_COMPACT).  The fonts read these too,
 * so build both after a change.  _font/check_charset.py checks that every key
 * has its glyph.  A program without an option still shows what it is sent from
 * a font with the glyphs, but a window that has such characters on it costs
 * full-size cells. */

/* The unaccented Greek letters, for the gr layout.  Off by default: the code
 * for them is only compiled in when this is 1 (about 100 bytes). */
#define XTERM_GREEK 0

/* Latin-2 letters (Polish, Czech, Slovak, Hungarian, Romanian, Croatian, ...)
 * and the Turkish letters of Latin-3 (the glyphs also have the Romanian
 * comma-below ones; the Latin-1 letters are in every font).  Off by default:
 * the table and its lookup are only compiled in when this is 1 (about 150
 * bytes). */
#define XTERM_LATIN2 0

/* Cyrillic letters (Russian, Ukrainian, Bulgarian, Serbian, ...).  Off by
 * default: the table and its lookup are only compiled in when this is 1 (about
 * 150 bytes). */
#define XTERM_CYRILLIC 0

/* The three options as bits.  The font records in its header which of them it
 * has the glyphs for (mkfont.c), and a program refuses a font that lacks one of
 * its own (xterm_init). */
#define XTERM_LANGS (!!XTERM_GREEK | !!XTERM_LATIN2 << 1 | !!XTERM_CYRILLIC << 2)
