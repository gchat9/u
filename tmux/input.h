#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"  /* MUX_PREFIX, ESC_TIMEOUT_MS */

/*
 * Commands issued by the prefix-key FSM to the main loop.
 */
typedef enum {
    CMD_NONE = 0,
    CMD_PASS_BYTE,      /* forward `byte` verbatim to the active PTY   */
    CMD_PASS_TWO,       /* forward two bytes (`byte` then `byte2`)      */
    CMD_NEW_WINDOW,     /* Ctrl-A c                                     */
    CMD_NEXT_WINDOW,    /* Ctrl-A n                                     */
    CMD_PREV_WINDOW,    /* Ctrl-A p                                     */
    CMD_SELECT_WINDOW,  /* Alt+0-9 → window index in `arg`             */
    CMD_DETACH,         /* Ctrl-A d                                     */
    CMD_QUIT,           /* Ctrl-A q  (kill mux)                        */
    CMD_CLOSE_WINDOW,   /* Ctrl-A x  (close current window)            */
    CMD_SCROLLBACK_ENTER,      /* Ctrl-A [     (enter scrollback mode)  */
    CMD_SCROLLBACK_ENTER_PGUP, /* Ctrl-A PgUp (enter + scroll one page)  */
    CMD_REDRAW,         /* Ctrl-A l  (force full redraw)               */
} InputCmd;

typedef struct {
    InputCmd cmd;
    uint8_t  byte;   /* valid when cmd == CMD_PASS_BYTE / CMD_PASS_TWO  */
    uint8_t  byte2;  /* valid when cmd == CMD_PASS_TWO                  */
    int      arg;    /* valid when cmd == CMD_SELECT_WINDOW             */
} InputEvent;

/*
 * Put the real terminal into raw mode.  Saves the original termios so
 * input_restore() can bring it back.
 */
void input_raw_mode(void);

/*
 * Restore the original terminal settings.
 */
void input_restore(void);

/*
 * Feed one byte from stdin through the prefix-key FSM.
 * Returns an InputEvent describing what to do.
 */
InputEvent input_feed(uint8_t byte);

/*
 * Returns true when the FSM is waiting to see whether a bare ESC is
 * the start of an Alt+key sequence or a standalone Escape keypress.
 * The main loop should use a short select() timeout (≈20ms) in this
 * state so the user's bare Esc is not held up noticeably.
 */
bool input_esc_pending(void);

/*
 * Called by the main loop when the ESC-disambiguation timer expires.
 * Resets the FSM to GROUND and returns an event that forwards the
 * bare ESC to the active PTY.
 */
InputEvent input_flush_esc(void);
