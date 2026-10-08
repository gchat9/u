/*
 * config.h — compile-time configuration for mux
 *
 * Edit this file to customise behaviour and appearance.
 * All colour values are xterm 256-colour palette indices (0-255).
 * See https://www.ditig.com/256-colors-cheat-sheet for a reference.
 */

#pragma once

#include <stdint.h>  /* uint8_t, for Style below */

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
 * to paste PRIMARY.  When a program has asked for the mouse these go to it
 * instead -- unless this modifier is held, which keeps the mouse for
 * copy and paste: 4 Shift, 8 Alt, 16 Control (add them to need several);
 * 0 leaves the mouse to the program for good. */
#ifndef MOUSE_SELECT_MOD
#define MOUSE_SELECT_MOD 4
#endif

/* ================================================================== */
/* Session                                                              */
/* ================================================================== */

/* Unix socket path for detach/attach.  A daemon listens here after
 * PREFIX+d; "mux attach" connects to it. */
#ifndef MUX_SOCKET_PATH
#define MUX_SOCKET_PATH "/run/utmux-root"
#endif

/* ================================================================== */
/* Core settings                                                        */
/* ================================================================== */

/* Maximum number of windows.  Slots are allocated lazily so raising
 * this has no cost until windows are actually created. */
#define MAX_WINDOWS     10

/* Prefix key (Ctrl-A = 0x01, Ctrl-B = 0x02, etc.) */
#define MUX_PREFIX      0x02

/* Milliseconds to wait after a bare ESC before deciding it is not the
 * start of an Alt+key sequence.  20ms is imperceptible to humans but
 * long enough for a terminal escape sequence to arrive in one burst. */
#define ESC_TIMEOUT_MS  20

/* Shell to exec in new windows.  NULL → use $SHELL → /bin/bash. */
#define DEFAULT_SHELL   NULL


/* ================================================================== */
/* Colour / style primitives                                            */
/* ================================================================== */

/*
 * These macros produce ANSI escape sequences as compile-time string
 * constants.  Because adjacent string literals are concatenated by the
 * C compiler, you can combine them freely:
 *
 *   _BG(234) _FG(214) _BOLD   →   "\033[48;5;234m\033[38;5;214m\033[1m"
 *
 * _FG / _BG accept 256-colour palette indices.
 * For the 8 standard colours you can use _FG_ANSI(0..7) / _BG_ANSI(0..7).
 */
/* _XSTR expands its argument before stringifying it.  A bare #n would
 * not: _FG(STATUS_BAR_FG) would yield the text "STATUS_BAR_FG" instead of
 * the number it stands for (and a terminal would read the resulting
 * "\033[38;5;S" as Scroll Up, wiping the screen). */
#define _STR(x)         #x
#define _XSTR(x)        _STR(x)
#define _FG(n)          "\033[38;5;" _XSTR(n) "m"
#define _BG(n)          "\033[48;5;" _XSTR(n) "m"
#define _FG_ANSI(n)     "\033[" _XSTR(n) "m"     /* n = 30-37 or 90-97      */
#define _BG_ANSI(n)     "\033[" _XSTR(n) "m"     /* n = 40-47 or 100-107    */
#define _BOLD           "\033[1m"
#define _DIM            "\033[2m"
#define _ITALIC         "\033[3m"
#define _UNDERLINE      "\033[4m"
#define _RESET          "\033[m"


/* ================================================================== */
/* Status bar colours                                                   */
/*                                                                      */
/* Naming mirrors tmux's option names so the mapping is obvious to     */
/* anyone familiar with tmux.conf:                                      */
/*                                                                      */
/*   status-left           → STATUS_LEFT_*                             */
/*   status-right          → STATUS_RIGHT_*                             */
/*   status (bar bg/fg)    → STATUS_BAR_*                              */
/*   window-status-style          → WINDOW_STATUS_*                    */
/*   window-status-current-style  → WINDOW_STATUS_CURRENT_*            */
/*   window-status-activity-style → WINDOW_STATUS_ACTIVITY_*           */
/*                                                                      */
/* Each style is one xterm 256-colour fg/bg pair, given as plain        */
/* numbers exactly once below, in two compile-time-only forms so        */
/* neither backend does any runtime parsing or conversion:              */
/*                                                                      */
/*   *_STYLE  a ready-made ANSI string (via _FG/_BG above), for the    *
 *            VT-to-VT backend and anywhere else building escape text   *
 *            at compile time (see main.c's scrollback banner).         *
 *   *_COLOR  the same fg/bg as a Style{fg,bg} struct literal -- the    *
 *            exact two bytes the X11 backend's per-cell colour         *
 *            buffers want (see vt.h's identical Cell.fg/bg encoding),  *
 *            written in directly with nothing to parse.                */
/* ================================================================== */

typedef struct { uint8_t fg, bg; } Style;

/* Background that fills the whole status bar (gaps, padding, etc.)    */
#define STATUS_BAR_FG 250
#define STATUS_BAR_BG 234
#define STATUS_BAR_STYLE  _FG(STATUS_BAR_FG) _BG(STATUS_BAR_BG)
#define STATUS_BAR_COLOR  ((Style){ STATUS_BAR_FG, STATUS_BAR_BG })

/* Left section: hostname                                               */
#define STATUS_LEFT_FG 78
#define STATUS_LEFT_BG 237
#define STATUS_LEFT_STYLE _FG(STATUS_LEFT_FG) _BG(STATUS_LEFT_BG)
#define STATUS_LEFT_COLOR ((Style){ STATUS_LEFT_FG, STATUS_LEFT_BG })

/* Right section: clock                                                 */
#define STATUS_RIGHT_FG 78
#define STATUS_RIGHT_BG 237
#define STATUS_RIGHT_STYLE _FG(STATUS_RIGHT_FG) _BG(STATUS_RIGHT_BG)
#define STATUS_RIGHT_COLOR ((Style){ STATUS_RIGHT_FG, STATUS_RIGHT_BG })

/* Inactive window tab                                                  */
#define WINDOW_STATUS_FG 78
#define WINDOW_STATUS_BG 0
#define WINDOW_STATUS_STYLE _FG(WINDOW_STATUS_FG) _BG(WINDOW_STATUS_BG)
#define WINDOW_STATUS_COLOR ((Style){ WINDOW_STATUS_FG, WINDOW_STATUS_BG })

/* Currently active (focused) window tab                               */
#define WINDOW_STATUS_CURRENT_FG 0
#define WINDOW_STATUS_CURRENT_BG 72
#define WINDOW_STATUS_CURRENT_STYLE \
    _FG(WINDOW_STATUS_CURRENT_FG) _BG(WINDOW_STATUS_CURRENT_BG)
#define WINDOW_STATUS_CURRENT_COLOR \
    ((Style){ WINDOW_STATUS_CURRENT_FG, WINDOW_STATUS_CURRENT_BG })

/* Dead/exited window tab  (tmux calls this "activity" but we use it   *
 * for zombie windows since we don't implement real activity flags yet) */
#define WINDOW_STATUS_ACTIVITY_FG 240
#define WINDOW_STATUS_ACTIVITY_BG 236
#define WINDOW_STATUS_ACTIVITY_STYLE \
    _FG(WINDOW_STATUS_ACTIVITY_FG) _BG(WINDOW_STATUS_ACTIVITY_BG)
#define WINDOW_STATUS_ACTIVITY_COLOR \
    ((Style){ WINDOW_STATUS_ACTIVITY_FG, WINDOW_STATUS_ACTIVITY_BG })

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
/* Window lifecycle                                                     */
/* ================================================================== */

/*
 * remain-on-exit behaviour (mirrors tmux's remain-on-exit option).
 *
 * 1 → keep the window open after the child exits, show a message, and
 *     let the user press Enter to respawn a new shell (current default).
 * 0 → close and free the window as soon as its child exits.  If it was
 *     the last window the mux exits too.
 */
#ifndef REMAIN_ON_EXIT
#define REMAIN_ON_EXIT  0
#endif
