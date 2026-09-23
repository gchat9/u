#pragma once

#include "vt.h"

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
