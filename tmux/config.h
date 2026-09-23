/*
 * config.h — compile-time configuration for mux
 *
 * Edit this file to customise behaviour and appearance.
 * All colour values are xterm 256-colour palette indices (0-255).
 * See https://www.ditig.com/256-colors-cheat-sheet for a reference.
 */

#pragma once

/* ================================================================== */
/* Session                                                              */
/* ================================================================== */

/* Unix socket path for detach/attach.  A daemon listens here after
 * PREFIX+d; "mux attach" connects to it. */
#ifndef MUX_SOCKET_PATH
#define MUX_SOCKET_PATH "/run/utmux"
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
#define _FG(n)          "\033[38;5;" #n "m"
#define _BG(n)          "\033[48;5;" #n "m"
#define _FG_ANSI(n)     "\033[" #n "m"     /* n = 30-37 or 90-97      */
#define _BG_ANSI(n)     "\033[" #n "m"     /* n = 40-47 or 100-107    */
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
/*   status-left           → STATUS_LEFT_STYLE                         */
/*   status-right          → STATUS_RIGHT_STYLE                        */
/*   status (bar bg/fg)    → STATUS_BAR_STYLE                          */
/*   window-status-style          → WINDOW_STATUS_STYLE                */
/*   window-status-current-style  → WINDOW_STATUS_CURRENT_STYLE        */
/*   window-status-activity-style → WINDOW_STATUS_ACTIVITY_STYLE       */
/* ================================================================== */

/* Background that fills the whole status bar (gaps, padding, etc.)    */
#define STATUS_BAR_STYLE \
    _BG(234) _FG(250)

/* Left section: hostname                                               */
#define STATUS_LEFT_STYLE \
    _BG(237) _FG(78)

/* Right section: clock                                                 */
#define STATUS_RIGHT_STYLE \
    _BG(237) _FG(78)

/* Inactive window tab                                                  */
#define WINDOW_STATUS_STYLE \
    _BG(0) _FG(78)

/* Currently active (focused) window tab                               */
#define WINDOW_STATUS_CURRENT_STYLE \
    _BG(72)  _FG(0)

/* Dead/exited window tab  (tmux calls this "activity" but we use it   *
 * for zombie windows since we don't implement real activity flags yet) */
#define WINDOW_STATUS_ACTIVITY_STYLE \
    _BG(236) _FG(240)

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
#define SCROLLBACK_BYTES    8192
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
