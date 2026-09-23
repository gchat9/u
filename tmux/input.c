#include "input.h"

#include <unistd.h>
#include <termios.h>
#include <string.h>

/* ================================================================== */
/* Terminal raw mode                                                    */
/* ================================================================== */

static struct termios saved_termios;
static bool           raw_active;

void input_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &saved_termios);

    struct termios raw = saved_termios;
    cfmakeraw(&raw);
    /*
     * VMIN=1, VTIME=0: blocking read, returns as soon as ≥1 byte arrives.
     */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_active = true;
}

void input_restore(void)
{
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        raw_active = false;
    }
}

/* ================================================================== */
/* Prefix-key finite-state machine                                      */
/* ================================================================== */

typedef enum {
    FSM_GROUND,      /* normal – pass bytes straight through             */
    FSM_COMMAND,     /* just saw MUX_PREFIX – waiting for command key    */
    FSM_ESC,         /* just saw bare ESC – disambiguate Alt vs sequence */
    FSM_PREFIX_ESC,  /* PREFIX then ESC – collecting escape sequence     */
    FSM_PREFIX_CSI,  /* PREFIX ESC [                                     */
    FSM_PREFIX_5,    /* PREFIX ESC [ 5                                   */
} FsmState;

static FsmState fsm_state;

InputEvent input_feed(uint8_t byte)
{
    InputEvent ev = { .cmd = CMD_NONE };

    switch (fsm_state) {

    case FSM_GROUND:
        if (byte == MUX_PREFIX) {
            fsm_state = FSM_COMMAND;
        } else if (byte == 0x1B) {
            /* Could be Alt+key or the start of a terminal escape
             * sequence (arrow keys, F-keys, etc.).  Wait for the
             * next byte to decide. */
            fsm_state = FSM_ESC;
        } else {
            ev.cmd  = CMD_PASS_BYTE;
            ev.byte = byte;
        }
        break;

    case FSM_ESC:
        fsm_state = FSM_GROUND;

        if (byte >= '0' && byte <= '9') {
            /* ESC + digit → Alt+digit → select window                 */
            /* Alt+1..9 → windows 0..8; Alt+0 → window 9              */
            ev.cmd = CMD_SELECT_WINDOW;
            ev.arg = (byte == '0') ? 9 : (byte - '1');
        } else {
            /* Not an Alt+digit shortcut.  Forward the ESC and the
             * current byte as two separate pass-through events.
             * The caller handles CMD_PASS_TWO by writing both bytes. */
            ev.cmd   = CMD_PASS_TWO;
            ev.byte  = 0x1B;
            ev.byte2 = byte;
        }
        break;

    case FSM_COMMAND:
        fsm_state = FSM_GROUND;

        switch (byte) {
        case MUX_PREFIX:
            ev.cmd  = CMD_PASS_BYTE;
            ev.byte = MUX_PREFIX;
            break;
        case 'c': case 'C':
            ev.cmd = CMD_NEW_WINDOW;
            break;
        case 'n': case 'N':
            ev.cmd = CMD_NEXT_WINDOW;
            break;
        case 'p': case 'P':
            ev.cmd = CMD_PREV_WINDOW;
            break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            /* Mirror Alt+number: 1-9 → windows 0-8, 0 → window 9 */
            ev.cmd = CMD_SELECT_WINDOW;
            ev.arg = (byte == '0') ? 9 : (byte - '1');
            break;
        case 'd': case 'D':
            ev.cmd = CMD_DETACH;
            break;
        case 'x': case 'X':
            ev.cmd = CMD_CLOSE_WINDOW;
            break;
        case '[':
            ev.cmd = CMD_SCROLLBACK_ENTER;
            break;
        case 'q': case 'Q':
            ev.cmd = CMD_QUIT;
            break;
        case 'l': case 'L': case 0x0C:
            ev.cmd = CMD_REDRAW;
            break;
        case 0x1B:
            /* PREFIX + ESC: start of PREFIX+PgUp (ESC[5~) or similar.
             * Hand off to the FSM_PREFIX_ESC chain. */
            fsm_state = FSM_PREFIX_ESC;
            return ev;   /* CMD_NONE; wait for rest of sequence */
        default:
            break;
        }
        break;

    case FSM_PREFIX_ESC:
        fsm_state = FSM_GROUND;
        if (byte == '[') { fsm_state = FSM_PREFIX_CSI; return ev; }
        break;

    case FSM_PREFIX_CSI:
        fsm_state = FSM_GROUND;
        if (byte == '5') { fsm_state = FSM_PREFIX_5; return ev; }
        break;

    case FSM_PREFIX_5:
        fsm_state = FSM_GROUND;
        if (byte == '~') ev.cmd = CMD_SCROLLBACK_ENTER_PGUP;
        break;
    }

    return ev;
}

bool input_esc_pending(void)
{
    return fsm_state == FSM_ESC;
}

InputEvent input_flush_esc(void)
{
    InputEvent ev = { .cmd = CMD_NONE };
    if (fsm_state == FSM_ESC) {
        fsm_state = FSM_GROUND;
        ev.cmd    = CMD_PASS_BYTE;
        ev.byte   = 0x1B;
    }
    return ev;
}
