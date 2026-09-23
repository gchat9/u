#pragma once
#include "config.h"

#define ROW_WRAPPED  0x01u  /* line reached right margin via autowrap */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Cell attributes                                                      */
/* ------------------------------------------------------------------ */

#define ATTR_BOLD      0x01
#define ATTR_DIM       0x02
#define ATTR_ITALIC    0x04
#define ATTR_UNDERLINE 0x08
#define ATTR_BLINK     0x10
#define ATTR_REVERSE   0x20
#define ATTR_INVIS     0x40
#define ATTR_STRIKE    0x80

/*
 * Cell.flags bits
 */
#define CELL_WIDE      0x01  /* first cell of a double-width character   */
#define CELL_WIDE_CONT 0x02  /* continuation cell of a double-width char */
#define CELL_FG_DFL    0x04  /* use terminal default foreground          */
#define CELL_BG_DFL    0x08  /* use terminal default background          */

/*
 * fg/bg colour encoding in Cell.fg / Cell.bg (uint8_t, 0-255):
 *   0-7    ANSI colours (SGR 30-37 / 40-47)
 *   8-15   bright ANSI colours (SGR 90-97 / 100-107)
 *   16-255 xterm 256-colour palette (SGR 38;5;N / 48;5;N)
 *   "default terminal colour" is signalled by CELL_FG_DFL / CELL_BG_DFL
 *   in Cell.flags; the fg/bg byte is then irrelevant.
 *
 * Screen.cur_fg / cur_bg (and saved variants) are uint16_t and use the
 * additional sentinel COLOR_DEFAULT=256 during SGR parsing; that value
 * is never stored directly in a Cell.
 */
#define COLOR_DEFAULT 256u

typedef struct {
    uint32_t ch;     /* Unicode codepoint; 0 treated as space             */
    uint8_t  fg;     /* 256-colour palette index (see CELL_FG_DFL)        */
    uint8_t  bg;     /* 256-colour palette index (see CELL_BG_DFL)        */
    uint8_t  attrs;  /* ATTR_* bitmask                                     */
    uint8_t  flags;  /* CELL_* bitmask (wide, wide_cont, fg/bg default)   */
} Cell;              /* 8 bytes — was 12                                   */

/* ------------------------------------------------------------------ */
/* Screen buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    Cell   **cells;         /* primary screen grid [rows][cols]          */
    Cell   **alt_cells;     /* alternate screen grid                     */
    int      rows, cols;

    /* Cursor */
    int      cur_row, cur_col;
    bool     cur_visible;
    bool     pending_wrap;  /* next printable char will wrap first       */

    /* Modes */
    bool     origin_mode;   /* DECOM  – cursor confined to scroll region */
    bool     auto_wrap;     /* DECAWM – wrap at right margin             */
    bool     app_cursor;    /* DECCKM – application cursor keys          */
    bool     bracketed_paste;
    bool     insert_mode;   /* IRM                                       */
    bool     in_alt_screen;

    /* Scroll region (0-indexed, inclusive) */
    int      scroll_top, scroll_bottom;

    /* Current SGR state applied to new characters */
    uint16_t cur_fg, cur_bg;
    uint8_t  cur_attrs;

    /* Saved cursor (ESC 7 / ESC 8) */
    int      saved_row, saved_col;
    uint16_t saved_fg, saved_bg;
    uint8_t  saved_attrs;
    bool     saved_wrap;

    /* Alternate screen saved cursor (used by ?1049h/l) */
    int      alt_saved_row, alt_saved_col;
    uint16_t alt_saved_fg,  alt_saved_bg;
    uint8_t  alt_saved_attrs;

    char     title[256];    /* set by OSC 0/1/2                         */
    uint8_t *row_flags;    /* per-row flags, e.g. ROW_WRAPPED           */

#if SCROLLBACK_ENABLED
    /* Plain-text scrollback: UTF-8 lines, trailing-whitespace stripped,
     * newline-terminated.  Oldest line dropped when full. */
    char    *scrollback;     /* heap buffer, SCROLLBACK_BYTES bytes       */
    int      scrollback_len; /* bytes currently used                      */
#endif
} Screen;

/* ------------------------------------------------------------------ */
/* VT parser                                                            */
/* ------------------------------------------------------------------ */

#define VT_MAX_PARAMS       16
#define VT_MAX_INTER         4
#define VT_MAX_OSC        1024
#define VT_MAX_COLS        512   /* for tab-stop table only             */

typedef enum {
    ST_GROUND,
    ST_ESC,
    ST_ESC_INTER,   /* ESC followed by intermediate bytes (e.g. charset) */
    ST_CSI_PARAM,   /* collecting CSI parameters and private-mode marker  */
    ST_CSI_INTER,   /* CSI intermediate bytes (before final)              */
    ST_OSC,         /* OSC string accumulation                            */
    ST_DCS,         /* DCS / APC / PM – ignored, wait for ST             */
} VTState;

typedef struct {
    VTState  state;

    /* CSI parameter accumulation */
    int      params[VT_MAX_PARAMS];
    int      num_params;        /* number of params seen so far           */
    char     inter[VT_MAX_INTER];
    int      num_inter;
    bool     priv;              /* CSI ? prefix                           */

    /* OSC string */
    char     osc[VT_MAX_OSC];
    int      osc_len;

    /* UTF-8 multi-byte carry buffer */
    uint8_t  utf8_buf[4];
    int      utf8_len;
    int      utf8_want;         /* total bytes expected for this seq      */

    /* Tab stops (bitset indexed by column, up to VT_MAX_COLS) */
    uint8_t  tabstops[VT_MAX_COLS / 8 + 1];

    /* Set when child asks for cursor-position report (CSI 6 n) */
    bool     cpr_requested;

    /* The virtual screen */
    Screen   scr;
} VTParser;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

void vt_init  (VTParser *p, int rows, int cols);
void vt_free  (VTParser *p);
void vt_resize(VTParser *p, int rows, int cols);
void vt_feed  (VTParser *p, const uint8_t *data, size_t len);

/*
 * Serialize the complete VT parser + screen state to fd.
 * Wire format: VTWire struct, osc buffer, ScreenWire struct,
 * primary cell data, has_alt flag, optional alt cell data.
 * Returns 0 on success, -1 on I/O error.
 */
int vt_serialize  (const VTParser *p, int fd);

/*
 * Deserialize from fd into p.  p is re-initialized internally at the
 * dimensions stored in the wire stream; caller should vt_resize() it
 * afterward if the new terminal is a different size.
 * Returns 0 on success, -1 on I/O error.
 */
int vt_deserialize(VTParser *p, int fd);
