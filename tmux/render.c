#include "render.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#ifdef X11_BACKEND
#include "x11_backend.h"
#endif

/* ================================================================== */
/* Output buffer                                                        */
/* ================================================================== */

#define OUTBUF_CAP (128 * 1024)
static char   outbuf[OUTBUF_CAP];
static int    outbuf_len;

static void out_flush(void)
{
    if (outbuf_len > 0) {
        (void)write(STDOUT_FILENO, outbuf, (size_t)outbuf_len);
        outbuf_len = 0;
    }
}

static void out_raw(const char *s, int n)
{
    if (outbuf_len + n > OUTBUF_CAP) out_flush();
    memcpy(outbuf + outbuf_len, s, (size_t)n);
    outbuf_len += n;
}

static void out_str(const char *s)
{
    out_raw(s, (int)strlen(s));
}

static void out_char(char c)
{
    if (outbuf_len >= OUTBUF_CAP) out_flush();
    outbuf[outbuf_len++] = c;
}

/* ================================================================== */
/* Emit helpers                                                         */
/* ================================================================== */

static void emit_move(RenderState *rs, int row, int col)
{
    if (rs->cursor_row == row && rs->cursor_col == col) return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    out_raw(buf, n);
    rs->cursor_row = row;
    rs->cursor_col = col;
}

/*
 * Emit the minimal SGR sequence to transition from the current render
 * state to (fg, bg, attrs).  We always start with SGR 0 (reset) and
 * re-specify everything so we never have to track partial state changes.
 */
static void emit_sgr(RenderState *rs, uint16_t fg, uint16_t bg, uint8_t attrs)
{
    if (fg == rs->cur_fg && bg == rs->cur_bg && attrs == rs->cur_attrs) return;

    char buf[64];
    int  pos = 0;

    buf[pos++] = '\033';
    buf[pos++] = '[';
    buf[pos++] = '0'; /* reset */

    if (attrs & ATTR_BOLD)      { buf[pos++]=';'; buf[pos++]='1'; }
    if (attrs & ATTR_DIM)       { buf[pos++]=';'; buf[pos++]='2'; }
    if (attrs & ATTR_ITALIC)    { buf[pos++]=';'; buf[pos++]='3'; }
    if (attrs & ATTR_UNDERLINE) { buf[pos++]=';'; buf[pos++]='4'; }
    if (attrs & ATTR_BLINK)     { buf[pos++]=';'; buf[pos++]='5'; }
    if (attrs & ATTR_REVERSE)   { buf[pos++]=';'; buf[pos++]='7'; }

    /* Foreground */
    if (fg < 8)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";%d", 30 + fg);
    else if (fg < 16)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";%d", 90 + fg - 8);
    else if (fg < 256)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";38;5;%d", fg);
    /* fg == COLOR_DEFAULT: reset already cleared it */

    /* Background */
    if (bg < 8)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";%d", 40 + bg);
    else if (bg < 16)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";%d", 100 + bg - 8);
    else if (bg < 256)
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";48;5;%d", bg);

    buf[pos++] = 'm';
    out_raw(buf, pos);

    rs->cur_fg    = fg;
    rs->cur_bg    = bg;
    rs->cur_attrs = attrs;
}

/* Encode a Unicode codepoint as UTF-8 and write to output buffer */
static void emit_utf8(uint32_t cp)
{
    char buf[4];
    int  n;
    if (cp < 0x80) {
        buf[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 |  (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >>  6) & 0x3F));
        buf[2] = (char)(0x80 |  (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 |  (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >>  6) & 0x3F));
        buf[3] = (char)(0x80 |  (cp & 0x3F));
        n = 4;
    }
    out_raw(buf, n);
}

/* ================================================================== */
/* Shadow buffer                                                        */
/* ================================================================== */

static Cell **alloc_shadow(int rows, int cols)
{
    Cell **s = calloc((size_t)rows, sizeof(Cell *));
    for (int r = 0; r < rows; r++) {
        s[r] = calloc((size_t)cols, sizeof(Cell));
        /* Initialise to an impossible value so the first render writes
         * every cell. */
        for (int c = 0; c < cols; c++) {
            s[r][c].ch = (uint32_t)-1;
        }
    }
    return s;
}

static void free_shadow(Cell **s, int rows)
{
    for (int r = 0; r < rows; r++) free(s[r]);
    free(s);
}

static bool cells_eq(const Cell *a, const Cell *b)
{
    return a->ch    == b->ch
        && a->fg    == b->fg
        && a->bg    == b->bg
        && a->attrs == b->attrs
        && a->flags == b->flags;
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void render_init(RenderState *rs, int rows, int cols)
{
    rs->rows           = rows;
    rs->cols           = cols;
    rs->shadow             = alloc_shadow(rows, cols);
    rs->shadow_row_wrapped = calloc((size_t)rows, sizeof(bool));
    rs->cursor_row         = -1;   /* unknown until first emit_move */
    rs->cursor_col         = -1;
    rs->cur_fg         = COLOR_DEFAULT;
    rs->cur_bg         = COLOR_DEFAULT;
    rs->cur_attrs      = 0;
    rs->cursor_visible = true;
}

void render_free(RenderState *rs)
{
    if (!rs->shadow) return;   /* already freed or never initialised */
    free_shadow(rs->shadow, rs->rows);
    rs->shadow = NULL;
    free(rs->shadow_row_wrapped);
    rs->shadow_row_wrapped = NULL;
    rs->rows   = 0;
    rs->cols   = 0;
}

void render_screen(RenderState *rs, const Screen *s)
{
#ifdef X11_BACKEND
    (void)rs;
    x11_backend_render(s);
    return;
#endif
    int rows = s->rows < rs->rows ? s->rows : rs->rows;
    int cols = s->cols < rs->cols ? s->cols : rs->cols;

    /* Fast path: if the cursor is already parked correctly and nothing
     * in the cell grid has changed, emit nothing at all. */
    bool cursor_ok = (rs->cursor_row    == s->cur_row  &&
                      rs->cursor_col    == s->cur_col  &&
                      rs->cursor_visible == s->cur_visible);
    if (cursor_ok) {
        bool dirty = false;
        for (int r = 0; r < rows && !dirty; r++)
            for (int c = 0; c < cols && !dirty; c++)
                if (!(s->cells[r][c].flags & CELL_WIDE_CONT) &&
                    !cells_eq(&s->cells[r][c], &rs->shadow[r][c]))
                    dirty = true;
        if (!dirty) return;
    }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const Cell *cell   = &s->cells[r][c];
            Cell       *shadow = &rs->shadow[r][c];

            /* Skip wide-char continuation cells – emitting the lead
             * cell already advanced the real cursor by two columns. */
            if (cell->flags & CELL_WIDE_CONT) {
                *shadow = *cell;
                continue;
            }

            if (cells_eq(cell, shadow)) continue;

            /* Move real cursor to this cell if needed */
            emit_move(rs, r, c);
            uint16_t efg = (cell->flags & CELL_FG_DFL) ? COLOR_DEFAULT : cell->fg;
            uint16_t ebg = (cell->flags & CELL_BG_DFL) ? COLOR_DEFAULT : cell->bg;
            emit_sgr(rs, efg, ebg, cell->attrs);

            if (cell->ch == 0 || cell->ch == ' ') {
                out_char(' ');
                if (cell->flags & CELL_WIDE) out_char(' '); /* fill continuation col */
            } else {
                emit_utf8(cell->ch);
            }

            /* Update shadow */
            *shadow = *cell;
            if ((cell->flags & CELL_WIDE) && c + 1 < cols) {
                Cell cont  = *cell;
                cont.flags = (cont.flags & ~CELL_WIDE) | CELL_WIDE_CONT;
                rs->shadow[r][c + 1] = cont;
            }

            /*
             * Update our idea of where the real cursor now is.
             * If we just wrote to the last column of a wrapped row, the
             * terminal auto-wraps to (r+1, 0) — record that so the next
             * emit_move can skip the CSI H.  For non-wrapped rows, set
             * cursor_col=-2 to force explicit repositioning as before.
             */
            int next_col = rs->cursor_col + ((cell->flags & CELL_WIDE) ? 2 : 1);
            if (next_col >= cols) {
                bool row_wrapped = s->row_flags && (s->row_flags[r] & ROW_WRAPPED);
                if (row_wrapped && r + 1 < rows) {
                    rs->cursor_row = r + 1;
                    rs->cursor_col = 0;
                    if (rs->shadow_row_wrapped) rs->shadow_row_wrapped[r] = true;
                } else {
                    rs->cursor_col = -2; /* force explicit move */
                }
            } else {
                rs->cursor_col = next_col;
            }
        }
    }

    /*
     * For wrapped rows whose wrap flag wasn't established by the diff
     * loop above (because the last cell was unchanged and not re-emitted),
     * force-emit the last character now.  This makes the terminal set its
     * per-line wrap flag so copy-paste joins wrapped lines correctly.
     */
    if (s->row_flags) {
        for (int r = 0; r < rows - 1; r++) {
            if (!(s->row_flags[r] & ROW_WRAPPED)) continue;
            if (rs->shadow_row_wrapped && rs->shadow_row_wrapped[r]) continue;
            /* Last cell of this row — skip wide-cont */
            int lc = cols - 1;
            if (s->cells[r][lc].flags & CELL_WIDE_CONT) continue;
            const Cell *cell = &s->cells[r][lc];
            emit_move(rs, r, lc);
            uint16_t efg = (cell->flags & CELL_FG_DFL) ? COLOR_DEFAULT : cell->fg;
            uint16_t ebg = (cell->flags & CELL_BG_DFL) ? COLOR_DEFAULT : cell->bg;
            emit_sgr(rs, efg, ebg, cell->attrs);
            if (cell->ch == 0 || cell->ch == ' ') out_char(' ');
            else emit_utf8(cell->ch);
            rs->shadow[r][lc] = *cell;
            rs->cursor_row = r + 1;
            rs->cursor_col = 0;
            if (rs->shadow_row_wrapped) rs->shadow_row_wrapped[r] = true;
        }
    }

    /* Restore SGR to default before placing cursor */
    emit_sgr(rs, COLOR_DEFAULT, COLOR_DEFAULT, 0);

    /*
     * Park cursor at the virtual position.
     *
     * We only need to hide the cursor during this final move if the
     * cursor is actually going to jump — i.e. the real cursor is not
     * already sitting at the target. Hiding it for the diff loop itself
     * is unnecessary: when only a few cells changed the cursor never
     * moves far enough to flicker visibly.  Hiding only around the park
     * step means typing a single character produces bare output with no
     * escape sequence bookends.
     */
    bool need_park = (rs->cursor_row != s->cur_row ||
                      rs->cursor_col != s->cur_col);
    bool need_show = (s->cur_visible && !rs->cursor_visible);

    if (need_park && rs->cursor_visible) {
        out_str("\033[?25l");
        rs->cursor_visible = false;
    }
    emit_move(rs, s->cur_row, s->cur_col);
    if (s->cur_visible && !rs->cursor_visible) {
        out_str("\033[?25h");
        rs->cursor_visible = true;
    }
    /* If visibility changed without a park (e.g. app toggled cursor) */
    if (!need_park && need_show) {
        out_str("\033[?25h");
        rs->cursor_visible = true;
    }
    /* Handle hide without move */
    if (!s->cur_visible && rs->cursor_visible) {
        out_str("\033[?25l");
        rs->cursor_visible = false;
    }

    out_flush();
}

void render_full_redraw(RenderState *rs, const Screen *s)
{
#ifdef X11_BACKEND
    (void)rs;
    x11_backend_render(s);
    return;
#endif
    /* Blank the physical screen in one shot, then let the diff loop
     * emit only cells that differ from the now-blank background.
     * This is far cheaper than writing a space to every cell. */
    /* Clear wrap-flag shadow: all rows need wrap flag re-established */
    if (rs->shadow_row_wrapped)
        memset(rs->shadow_row_wrapped, 0,
               (size_t)rs->rows * sizeof(bool));
    out_str("\033[2J\033[H");
    out_flush();

    /* Tell our accounting that the real cursor is now at (0,0) and
     * that all shadow cells are stale so every non-space gets redrawn. */
    rs->cursor_row = 0;
    rs->cursor_col = 0;

    /* Invalidate the entire shadow so every cell gets re-emitted */
    for (int r = 0; r < rs->rows; r++)
        for (int c = 0; c < rs->cols; c++)
            rs->shadow[r][c].ch = (uint32_t)-1;

    render_screen(rs, s);
}

void render_invalidate_cursor(RenderState *rs)
{
    rs->cursor_row = -1;
    rs->cursor_col = -1;
}
