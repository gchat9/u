#include "x11_backend.h"
#include "../tmux/status.h"

#include "../_lib/xterm.h"
#include "../_font/charset.h"
#include "../_font/emoji_charset.h"

#include <stdint.h>
#include <string.h>

int x11_backend_init(void)
{
    int rc = xterm_init();
    if (rc == 0) {
        static const char title[] = "u-tmux";
        xterm_set_title(title, sizeof title - 1);
    }
    return rc;
}

int x11_backend_rows(void) { return xterm_rows(); }
int x11_backend_columns(void) { return xterm_columns(); }
int x11_backend_wait(int timeout_ms) { return xterm_wait(timeout_ms); }
int x11_backend_fd(void) { return xterm_fd(); }
int x11_backend_read_input(uint8_t *buf, int cap) { return xterm_read_key(buf, cap); }
int x11_backend_close_requested(void) { return xterm_close_requested(); }

/* Resolve a Cell's colour down to two xterm-256-palette index bytes,
 * plus a small structural-attr bitmask (drawn as extra pixels, not a
 * colour swap — see xterm.c's fill_row).
 * CELL_FG_DFL/CELL_BG_DFL -> 15/0 (this backend's default fg/bg, see
 * xterm.c's build_palette). ATTR_BOLD brightens an 0-7 fg into its
 * 8-15 counterpart (there's no separate bold glyph, so colour is the
 * only bold cue available). Slot 15 is this backend's theme foreground
 * (C_FG), not real white, so a bold fg that lands on it -- default
 * colour, or white -- is moved to slot 231, the 6x6x6 cube's (255,255,
 * 255): bold default-coloured text shows as bright white, #ffffff.
 * (Plain bright white, SGR 97, stays the theme foreground: see
 * build_palette.) ATTR_REVERSE swaps the pair last, matching
 * how a real terminal composites reverse video. ATTR_UNDERLINE and
 * ATTR_STRIKE map straight through to XTERM_ATTR_UNDERLINE/STRIKE.
 * TODO: ATTR_DIM, ATTR_ITALIC, ATTR_BLINK, ATTR_INVIS aren't rendered
 * by this backend yet. */
static void resolve_colors(const Cell *cell, uint8_t *out_fg, uint8_t *out_bg,
                           uint8_t *out_attr)
{
    uint8_t fg = (cell->flags & CELL_FG_DFL) ? 15 : cell->fg;
    uint8_t bg = (cell->flags & CELL_BG_DFL) ? 0  : cell->bg;
    if (cell->attrs & ATTR_BOLD) {
        if (fg < 8) fg = (uint8_t)(fg + 8);
        if (fg == 15) fg = 231;
    }
    if (cell->attrs & ATTR_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
    *out_fg = fg; *out_bg = bg;

    uint8_t attr = 0;
    if (cell->attrs & ATTR_UNDERLINE) attr |= XTERM_ATTR_UNDERLINE;
    if (cell->attrs & ATTR_STRIKE)    attr |= XTERM_ATTR_STRIKE;
    *out_attr = attr;
}

/* Maps a decoded Unicode codepoint to this font's glyph slot (see
 * charset.h): ch==0 (an empty/never-written cell) reads as a blank
 * space. Returns -1 if the charset has no glyph for it, in which case
 * the caller stores slot 0 and sets XTERM_ATTR_MISSING on the cell, and
 * xterm draws a placeholder box (rather than substituting some
 * other glyph, which would silently change what's on screen). Slot 0
 * is a valid slot number for a real space, so it's only ever meaningful
 * alongside that flag. */
static inline int glyph_slot(uint32_t ch)
{
    return charset_slot(ch ? ch : ' ');
}

/* Glyph slot and attribute for one half of a double-width character:
 * a fullwidth form draws its narrow twin's glyph spread over two cells,
 * anything else is an emoji (index 255, a placeholder box, if we have
 * no bitmap for it).  `right` selects the right-hand cell. */
static int wide_glyph(uint32_t wch, int right, uint8_t *attr)
{
    uint32_t twin = charset_fullwidth_alias(wch);
    if (twin) {
        *attr = right ? XTERM_ATTR_FW_R : XTERM_ATTR_FW_L;
        return glyph_slot(twin);
    }
    int g = emoji_index(wch); if (g < 0) g = 255;
    *attr = right ? XTERM_ATTR_EMOJI_R : XTERM_ATTR_EMOJI_L;
    return g;
}

void x11_backend_render(const Screen *s)
{
    uint8_t *fb  = xterm_framebuffer();
    uint8_t *fgb = xterm_fg_buffer();
    uint8_t *bgb = xterm_bg_buffer();
    uint8_t *atb = xterm_attr_buffer();
    int cols = xterm_columns();
    int rows = xterm_rows();
    int h = s->rows < rows ? s->rows : rows;
    int w = s->cols < cols ? s->cols : cols;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            const Cell *cell = &s->cells[r][c];
            uint32_t ch = cell->ch;
            int i = r * cols + c;
            /* A wide cell is a CELL_WIDE cell plus a CELL_WIDE_CONT cell
             * (see vt.c put_char), drawn as two halves of one thing: an
             * emoji, or a fullwidth punctuation mark standing in for its
             * ASCII twin's glyph. Both halves store the same glyph byte
             * and flag which half they are. A wide character with
             * neither a bitmap nor an ASCII twin gets emoji index 255,
             * which xterm draws as a two-cell placeholder box. A
             * continuation cell whose left neighbour isn't wide
             * (orphaned by an overwrite) falls back to a blank. */
            uint32_t wch = 0; int right = 0;
            if (cell->flags & CELL_WIDE) wch = ch;
            else if ((cell->flags & CELL_WIDE_CONT) && c > 0 &&
                     (s->cells[r][c-1].flags & CELL_WIDE)) {
                wch = s->cells[r][c-1].ch; right = 1;
            }
            uint8_t wattr = 0;
            int g = wch ? wide_glyph(wch, right, &wattr) : glyph_slot(ch);
            fb[i] = g < 0 ? 0 : (uint8_t)g;
            resolve_colors(cell, &fgb[i], &bgb[i], &atb[i]);
            if (wattr)      atb[i] |= wattr;   /* OR: keeps underline/strike */
            else if (g < 0) atb[i] |= XTERM_ATTR_MISSING;  /* after resolve_colors, which sets atb */
        }
        for (int c = w; c < cols; c++) {
            int i = r * cols + c;
            fb[i] = (uint8_t)glyph_slot(0); fgb[i] = 15; bgb[i] = 0; atb[i] = 0;
        }
    }
    /* This is called on every content update (unlike the status bar,
     * which is deliberately redrawn far less often — see main.c), so
     * it's the right place to keep the cursor current: set it from the
     * Screen we were just handed, every time, rather than needing a
     * separate call that could easily end up firing before or after
     * the wrong render and lag a frame behind. */
    xterm_set_cursor(s->cur_row, s->cur_col);
    xterm_render();
}

/* Context for x11_emit: tracks where the next piece of text lands. */
typedef struct {
    uint8_t *fb, *fgb, *bgb, *atb;
    int row, xcols, cols, col;
} X11StatusCtx;

/* X11 emit: write each byte's glyph + the piece's own fg/bg straight
 * into the per-cell buffers. Style already IS the two bytes those
 * buffers want (see config.h), so there's nothing to parse or convert
 * -- this is the zero-conversion half promised there. */
static void x11_emit(void *vctx, const char *s, int n, Style style)
{
    X11StatusCtx *ctx = vctx;
    for (int i = 0; i < n && ctx->col < ctx->cols; i++, ctx->col++) {
        int idx = ctx->row * ctx->xcols + ctx->col;
        int g = glyph_slot((uint8_t)s[i]);
        ctx->fb[idx]  = g < 0 ? 0 : (uint8_t)g;
        ctx->fgb[idx] = style.fg;
        ctx->bgb[idx] = style.bg;
        ctx->atb[idx] = g < 0 ? XTERM_ATTR_MISSING : 0;
    }
}

void x11_backend_status(int row, int cols, int active,
                        pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    int xcols = xterm_columns();
    int xrows = xterm_rows();
    if (row < 0 || row >= xrows) return;
    if (cols > xcols) cols = xcols;

    X11StatusCtx ctx = {
        .fb = xterm_framebuffer(), .fgb = xterm_fg_buffer(),
        .bgb = xterm_bg_buffer(),  .atb = xterm_attr_buffer(),
        .row = row, .xcols = xcols, .cols = cols, .col = 0,
    };

    /* Blank the row first: status_layout()'s own padding math already
     * covers exactly `cols` columns when content fits, but this is a
     * cheap defensive backstop (e.g. the pathological content-wider-
     * than-cols case) rather than relying on that invariant alone. */
    for (int c = 0; c < cols; c++) {
        int i = row * xcols + c;
        ctx.fb[i] = (uint8_t)glyph_slot(0); ctx.fgb[i] = 15; ctx.bgb[i] = 0; ctx.atb[i] = 0;
    }

    status_layout(x11_emit, &ctx, cols, active, child_pids, wins_exist, wins_alive);
    xterm_render();
}

/* ── Plain-text view (the scrollback viewer) ──────────────────────────
 * The scrollback is text, not cells, so it can't go through
 * x11_backend_render().  These draw UTF-8 into the same cell buffers and
 * behave like writing to a terminal: x11_backend_text_line() appends at a
 * text cursor, wraps long lines and scrolls the whole screen up when it
 * runs off the bottom, exactly as the plain build's output to a real
 * terminal does.  Colours come per call, as palette indices. */
static int tr_row, tr_col;

static void blank_cells(int i, int n, Style st)
{
    memset(xterm_framebuffer() + i, glyph_slot(0), (size_t)n);
    memset(xterm_fg_buffer() + i, st.fg, (size_t)n);
    memset(xterm_bg_buffer() + i, st.bg, (size_t)n);
    memset(xterm_attr_buffer() + i, 0,  (size_t)n);
}

void x11_backend_text_fill(int row, Style st)
{
    if (row >= 0 && row < xterm_rows())
        blank_cells(row * xterm_columns(), xterm_columns(), st);
}

void x11_backend_text_begin(void)
{
    blank_cells(0, xterm_rows() * xterm_columns(), (Style){ 15, 0 });
    tr_row = tr_col = 0;
}

/* Next UTF-8 character of [*p, end); tolerant of truncated or stray
 * bytes (a stray byte is returned as itself).  Zero-width characters
 * (see vt.c's put_char) come back as 0 and take no cell. */
static uint32_t next_char(const char **p, const char *end, int *width)
{
    const unsigned char *u = (const unsigned char *)*p;
    uint32_t cp = u[0]; int len = 1;
    if      (cp >= 0xF0) { cp &= 0x07; len = 4; }
    else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
    else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
    if (len > end - *p) len = 1, cp = u[0];
    for (int k = 1; k < len; k++) cp = cp << 6 | (u[k] & 0x3F);
    *p += len;
    *width = (cp >= 0x231A && (emoji_wide(cp) || charset_fullwidth(cp))) ? 2 : 1;
    if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0x200B && cp <= 0x200F)) return 0;
    return cp;
}

/* Store `cp` (width 1 or 2) at the given cell and, if wide, the next. */
static void put_text_char(int i, uint32_t cp, int width, Style st)
{
    uint8_t *fb = xterm_framebuffer(), *atb = xterm_attr_buffer();
    for (int h = 0; h < width; h++) {
        uint8_t attr = 0;
        int g = width == 2 ? wide_glyph(cp, h, &attr) : glyph_slot(cp);
        fb[i + h] = g < 0 ? 0 : (uint8_t)g;
        atb[i + h] = (g < 0 && !attr) ? XTERM_ATTR_MISSING : attr;
        xterm_fg_buffer()[i + h] = st.fg;
        xterm_bg_buffer()[i + h] = st.bg;
    }
}

/* Draw up to n bytes of text at (row, col), clipped to the row, no wrap. */
void x11_backend_text_at(int row, int col, const char *s, int n, Style st)
{
    int cols = xterm_columns();
    if (row < 0 || row >= xterm_rows()) return;
    for (const char *end = s + n; s < end; ) {
        int w; uint32_t cp = next_char(&s, end, &w);
        if (!cp) continue;
        if (col + w > cols) break;
        put_text_char(row * cols + col, cp, w, st);
        col += w;
    }
}

/* Append a line at the text cursor (wrapping), then move to the next
 * row, scrolling the screen up by one row if that runs off the bottom. */
static void text_newline(void)
{
    int cols = xterm_columns(), rows = xterm_rows();
    tr_col = 0;
    if (++tr_row < rows) return;
    tr_row = rows - 1;
    memmove(xterm_framebuffer(),   xterm_framebuffer()   + cols, (size_t)(rows - 1) * cols);
    memmove(xterm_fg_buffer(),     xterm_fg_buffer()     + cols, (size_t)(rows - 1) * cols);
    memmove(xterm_bg_buffer(),     xterm_bg_buffer()     + cols, (size_t)(rows - 1) * cols);
    memmove(xterm_attr_buffer(),   xterm_attr_buffer()   + cols, (size_t)(rows - 1) * cols);
    blank_cells(tr_row * cols, cols, (Style){ 15, 0 });
}

void x11_backend_text_line(const char *s, int n, Style st)
{
    int cols = xterm_columns();
    for (const char *end = s + n; s < end; ) {
        int w; uint32_t cp = next_char(&s, end, &w);
        if (!cp) continue;
        if (tr_col + w > cols) text_newline();
        put_text_char(tr_row * cols + tr_col, cp, w, st);
        tr_col += w;
    }
    text_newline();
}

/* Show what was drawn above; the text view has no cursor. */
void x11_backend_text_end(void)
{
    xterm_set_cursor(-1, -1);
    xterm_render();
}
