#pragma once

#include "../_lib/vt.h"

/*
 * RenderState tracks what we believe is currently displayed on the real
 * terminal so we can emit only the differences.
 */
typedef struct {
    Cell   **shadow;        /* last-rendered content [rows][cols]        */
    int      rows, cols;

    /* Real-terminal cursor position as we believe it to be */
    int      cursor_row, cursor_col;
    bool    *shadow_row_wrapped; /* which rows the terminal has wrap-flag for */
    uint32_t *row_hash;     /* 2*rows scratch: row hashes, new then shadow  */

    /* The status row: one more row of cells, below the screen, drawn by
     * render_screen() like the rest.  shadow[rows] is its shadow. */
    Cell    *status;        /* cols cells, filled by the caller            */
    int      status_row;    /* terminal row it is drawn on; -1: none       */

    /* Real-terminal SGR state */
    uint16_t cur_fg, cur_bg;
    uint8_t  cur_attrs;

    bool     cursor_visible;
} RenderState;

void render_init       (RenderState *rs, int rows, int cols);
void render_free       (RenderState *rs);

/*
 * Diff the virtual Screen against the shadow and emit the minimum
 * escape sequences needed to bring the real terminal into sync.
 */
void render_screen     (RenderState *rs, const Screen *s);

/*
 * Claim the status row for terminal row `row` (0-based) and return its cells,
 * rs->cols of them, all blank: the caller fills in the ones it wants and the
 * next render_screen() draws what differs from what the row showed, so a
 * clock tick or a changed label costs a few bytes rather than the whole row.
 * The row is drawn on every render_screen() from then on, with the cells as
 * last filled in.
 */
Cell *render_status_row(RenderState *rs, int row);

/*
 * Force a complete repaint by invalidating the shadow first.
 */
void render_full_redraw(RenderState *rs, const Screen *s);

/*
 * Tell the renderer that something wrote to the real terminal outside
 * its knowledge (e.g. the status bar), so it must re-emit the cursor
 * move on the next render_screen() call even if it thinks the cursor
 * is already in the right place.
 */
void render_invalidate_cursor(RenderState *rs);
