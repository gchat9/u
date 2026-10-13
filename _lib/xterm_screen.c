/*
 * xterm_screen.c -- a Screen (vt.h's cell grid) in an xterm.c window, and the
 * terminal behaviour that needs to know its contents: drawing it, the mouse
 * (reported to the program, or copy and paste), selection and clipboard.
 * Everything a program that shows a Screen needs besides the window itself
 * (xterm.c) and the parser that fills the Screen (vt.c); used by xtmux, see
 * xterm_screen.h.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xterm_screen.h"
#include "../_font/charset.h"
#include "../_font/emoji_charset.h"

/* Resolve a Cell's colour down to two xterm-256-palette index bytes,
 * plus a small structural-attr bitmask (drawn as extra pixels, not a
 * colour swap -- see xterm.c's fill_row).  A default colour (CELL_FG_DFL /
 * CELL_BG_DFL) is the palette slot XTERM_FG_DEFAULT / XTERM_BG_DEFAULT
 * (xterm.h).  ATTR_BOLD brightens an 0-7 fg into its 8-15 counterpart
 * (there's no separate bold glyph, so colour is the only bold cue
 * available), so bold default text becomes slot 15, #ffffff.
 * ATTR_REVERSE swaps the pair last, matching how a real terminal composites
 * reverse video.  ATTR_UNDERLINE and ATTR_STRIKE map straight through to
 * XTERM_ATTR_UNDERLINE/STRIKE.
 * TODO: ATTR_DIM, ATTR_ITALIC, ATTR_BLINK, ATTR_INVIS aren't rendered
 * by this backend yet. */
static void resolve_colors(const Cell *cell, uint8_t *out_fg, uint8_t *out_bg,
                           uint8_t *out_attr)
{
    uint8_t fg = (cell->flags & CELL_FG_DFL) ? XTERM_FG_DEFAULT : cell->fg;
    uint8_t bg = (cell->flags & CELL_BG_DFL) ? XTERM_BG_DEFAULT : cell->bg;
    if ((cell->attrs & ATTR_BOLD) && fg < 8) fg = (uint8_t)(fg + 8);
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
int xscr_glyph_slot(uint32_t ch)
{
    return charset_slot(ch ? ch : ' ');
}

/* Glyph slot and attribute for one half of a double-width character:
 * one with a bitmap of its own in the font's wide glyphs (ツ), a
 * fullwidth form drawing its narrow twin's glyph spread over two cells,
 * or anything else is an emoji (index 255, a placeholder box, if we have
 * no bitmap for it).  `right` selects the right-hand cell. */
int xscr_wide_glyph(uint32_t wch, int right, uint8_t *attr)
{
    int ws = charset_wide_slot(wch);
    if (ws >= 0) {
        *attr = (right ? XTERM_ATTR_FW_R : XTERM_ATTR_FW_L) | XTERM_ATTR_WIDE;
        return ws;
    }
    uint32_t twin = charset_fullwidth_alias(wch);
    if (twin) {
        *attr = right ? XTERM_ATTR_FW_R : XTERM_ATTR_FW_L;
        return xscr_glyph_slot(twin);
    }
    int g = emoji_index(wch); if (g < 0) g = 255;
    *attr = right ? XTERM_ATTR_EMOJI_R : XTERM_ATTR_EMOJI_L;
    return g;
}

/* The text selection, if any: where the mouse went down (sel_a) and where
 * it is now (sel_b), as row << 16 | col, see "Selection and clipboard"
 * below.  sel_mode says what they select (SM_*); sel_lo..sel_hi is the
 * cells that makes, worked out by sel_calc when either end moves: in
 * reading order, or for SM_RECT the corners of a rectangle.  sel_on: it is
 * drawn (in reverse video); sel_drag: the drag that is making it is still
 * going. */
enum { SM_CHAR, SM_RECT, SM_WORD, SM_LINE };
static uint8_t  sel_on, sel_drag, sel_mode;
static unsigned sel_a, sel_b, sel_lo, sel_hi;
#define SEL_KEY(r, c) ((unsigned)(r) << 16 | (unsigned)(c))

static int sel_hit(int r, int c)
{
    unsigned k = SEL_KEY(r, c);
    if (sel_mode == SM_RECT)
        return (unsigned)r >= sel_lo >> 16 && (unsigned)r <= sel_hi >> 16 &&
               (unsigned)c >= (sel_lo & 0xFFFF) && (unsigned)c <= (sel_hi & 0xFFFF);
    return k >= sel_lo && k <= sel_hi;
}

void xscr_render(const Screen *s)
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
             * emoji, a glyph with a two-cell bitmap of its own, or a
             * fullwidth punctuation mark standing in for its ASCII
             * twin's glyph. Both halves store the same glyph byte
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
            int g = wch ? xscr_wide_glyph(wch, right, &wattr) : xscr_glyph_slot(ch);
            fb[i] = g < 0 ? 0 : (uint8_t)g;
            resolve_colors(cell, &fgb[i], &bgb[i], &atb[i]);
            if (sel_on && sel_hit(r, c)) {     /* selected: reverse video */
                uint8_t t = fgb[i]; fgb[i] = bgb[i]; bgb[i] = t;
            }
            if (wattr)      atb[i] |= wattr;   /* OR: keeps underline/strike */
            else if (g < 0) atb[i] |= XTERM_ATTR_MISSING;  /* after resolve_colors, which sets atb */
        }
        for (int c = w; c < cols; c++) {
            int i = r * cols + c;
            fb[i] = (uint8_t)xscr_glyph_slot(0); fgb[i] = XTERM_FG_DEFAULT; bgb[i] = XTERM_BG_DEFAULT; atb[i] = 0;
        }
    }
    /* A frame shorter than the window (an observed session smaller than
     * our window) must not leave older rows below it.  The last row is
     * the status bar's, drawn separately and far less often, so it is
     * left alone: for our own screens h is already rows - 1 and this
     * loop does nothing. */
    for (int r = h; r < rows - 1; r++)
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            fb[i] = (uint8_t)xscr_glyph_slot(0); fgb[i] = XTERM_FG_DEFAULT; bgb[i] = XTERM_BG_DEFAULT; atb[i] = 0;
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

/* ── Mouse reports ────────────────────────────────────────────────────── */


static int put_dec(uint8_t *o, unsigned v)
{
    uint8_t t[5]; int n = 0, k = 0;
    do { t[n++] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = t[--n];
    return k;
}

#ifdef MOUSE_UTF8_URXVT_ENCODINGS
static int put_utf8(uint8_t *o, unsigned v)     /* v < 0x800 */
{
    if (v < 0x80) { o[0] = (uint8_t)v; return 1; }
    o[0] = (uint8_t)(0xC0 | v >> 6); o[1] = (uint8_t)(0x80 | (v & 0x3F));
    return 2;
}
#endif

/* The escape sequence for pointer event *mp, as a program that asked for
 * the mouse with ?1000/?1002/?1003 (mode 1/2/3) wants it: ?1006 (enc 2
 * SGR; 0 the classic form), and with MOUSE_UTF8_URXVT_ENCODINGS
 * ?1005/?1015 (enc 1 UTF-8, 3 urxvt).  Only events inside the rows x cols
 * area the program sees count.  Puts at most 40 bytes in `out` and
 * returns their number: 0 for an event that is not reported -- wrong mode,
 * outside the area, or not representable (the classic form stops at
 * column/row 223). */
static int mouse_encode(const XtermMouse *mp, int mode, int enc, int rows, int cols, uint8_t *out)
{
    XtermMouse m = *mp;
    if (!mode || m.row >= rows || m.col >= cols) return 0;

    int btn = m.button, cb, release = 0;
    if (m.kind == 2) {                      /* motion */
        if (mode < 2 || (mode == 2 && !btn)) return 0;
        cb = 32 + (btn ? btn - 1 : 3);      /* 3: no button held */
    } else if (btn >= 4 && btn <= 7) {      /* wheel: a press, never a release */
        if (m.kind) return 0;
        cb = 64 + btn - 4;
    } else if (btn >= 1 && btn <= 3) {
        cb = btn - 1; release = m.kind;
    } else {
        return 0;                           /* extra buttons: not reported */
    }
    cb |= m.mods;
    unsigned x = m.col + 1u, y = m.row + 1u;

    uint8_t *o = out;
    *o++ = 033; *o++ = '[';
    if (enc == 2) {                         /* SGR: the button survives release */
        *o++ = '<'; o += put_dec(o, (unsigned)cb); *o++ = ';';
        o += put_dec(o, x); *o++ = ';'; o += put_dec(o, y);
        *o++ = release ? 'm' : 'M';
        return (int)(o - out);
    }
    if (release) cb = 3 | m.mods;           /* the others: "a button went up" */
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
    if (enc == 3) {                         /* urxvt: decimal, button + 32 */
        o += put_dec(o, (unsigned)cb + 32); *o++ = ';';
        o += put_dec(o, x); *o++ = ';'; o += put_dec(o, y);
        *o++ = 'M';
        return (int)(o - out);
    }
#endif
    *o++ = 'M';
#ifdef MOUSE_UTF8_URXVT_ENCODINGS
    if (enc == 1) {                         /* UTF-8 coordinates, up to 2015 */
        if (x + 32 >= 0x800 || y + 32 >= 0x800) return 0;
        o += put_utf8(o, (unsigned)cb + 32);
        o += put_utf8(o, x + 32); o += put_utf8(o, y + 32);
    } else
#endif
    {                                       /* classic: one byte each */
        if (x > 223 || y > 223) return 0;
        *o++ = (uint8_t)(cb + 32); *o++ = (uint8_t)(x + 32); *o++ = (uint8_t)(y + 32);
    }
    return (int)(o - out);
}

/* ── Selection and clipboard ───────────────────────────────────────────
 * Mouse policy: while the displayed program has not asked for the mouse
 * (mode 0), or MOUSE_SELECT_MOD (xterm_conf.h) is held when a button goes
 * down, the mouse belongs to us -- left drag selects text and copies it to
 * PRIMARY and CLIPBOARD (double click: a word, triple click: a line, and
 * dragging after either extends it by words or lines; Alt held: the
 * rectangle between the two ends), a right click pastes PRIMARY.  Otherwise the
 * events are passed to the program.  Which of the two it is is decided
 * when a button goes down and holds until it comes up again. */

typedef void (*SendFn)(const uint8_t *, size_t, void *);

static uint8_t grab, grab_btn;      /* a button is held: 1 the program's, 2 ours */
static char   *sel_buf;             /* the copied text (xterm_selection_set serves it) */

/* Where pasted text goes, as last registered by the code that handles the
 * pointer (see xscr_pointer): paste text arrives some time after
 * it was asked for. */
static struct { SendFn send; void *ctx; uint8_t bracketed, started, after_cr; } paste;

static int utf8_encode(uint8_t *o, uint32_t cp)
{
    if (cp < 0x80)    { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800)   { o[0] = (uint8_t)(0xC0 | cp >> 6);
                        o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (uint8_t)(0xE0 | cp >> 12);
                        o[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
                        o[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (uint8_t)(0xF0 | cp >> 18); o[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* Is the cell at (r, c) of s blank (a space or empty), as opposed to part
 * of a word?  The second half of a wide character is not blank. */
static int blank(const Screen *s, int r, int c)
{
    const Cell *x = &s->cells[r][c];
    return (!x->ch || x->ch == ' ') && !(x->flags & CELL_WIDE_CONT);
}

/* The end of the word at key k, d = -1 for its start, 1 for its end: a word
 * is a run of cells that are all blank or all not, within the row. */
static unsigned word_edge(const Screen *s, unsigned k, int d)
{
    int r = (int)(k >> 16), c = (int)(k & 0xFFFF);
    if (r >= s->rows || c >= s->cols) return k;
    int t = blank(s, r, c);
    while (c + d >= 0 && c + d < s->cols && blank(s, r, c + d) == t) c += d;
    return SEL_KEY(r, c);
}

/* Set sel_lo/sel_hi from sel_a, sel_b and sel_mode. */
static void sel_calc(const Screen *s)
{
    unsigned lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
    if (sel_mode == SM_RECT) {
        unsigned ra = sel_a >> 16, rb = sel_b >> 16, ca = sel_a & 0xFFFF, cb = sel_b & 0xFFFF;
        lo = SEL_KEY(ra < rb ? ra : rb, ca < cb ? ca : cb);
        hi = SEL_KEY(ra < rb ? rb : ra, ca < cb ? cb : ca);
    } else if (sel_mode == SM_WORD) {
        lo = word_edge(s, lo, -1); hi = word_edge(s, hi, 1);
    } else if (sel_mode == SM_LINE) {
        lo &= ~0xFFFFu; hi |= 0xFFFF;               /* (clamped to the width below) */
    }
    sel_lo = lo; sel_hi = hi;
    sel_on = sel_mode >= SM_WORD || sel_a != sel_b;
}

/* The selected text (sel_lo..sel_hi) from s: in reading order, or by rows
 * of the rectangle; trailing blanks of each line dropped, a newline between
 * lines unless the line wrapped.  malloc'd, *len bytes (not terminated);
 * NULL if there is nothing. */
static char *sel_extract(const Screen *s, size_t *len)
{
    int rect = sel_mode == SM_RECT;
    int r0 = (int)(sel_lo >> 16), c0 = (int)(sel_lo & 0xFFFF);
    int r1 = (int)(sel_hi >> 16), c1 = (int)(sel_hi & 0xFFFF);
    if (r1 >= s->rows) { r1 = s->rows - 1; c1 = s->cols - 1; }
    if (r0 > r1) return NULL;
    char *buf = malloc((size_t)(r1 - r0 + 1) * ((size_t)s->cols * 4 + 1) + 1);
    if (!buf) return NULL;
    size_t n = 0;
    for (int r = r0; r <= r1; r++) {
        int a = rect || r == r0 ? c0 : 0, b = rect || r == r1 ? c1 : s->cols - 1;
        if (b >= s->cols) b = s->cols - 1;
        int wrapped = !rect && r < r1 && s->row_flags && (s->row_flags[r] & ROW_WRAPPED);
        if (!wrapped)
            while (b >= a && (s->cells[r][b].ch == 0 || s->cells[r][b].ch == ' ')) b--;
        for (int c = a; c <= b; c++) {
            const Cell *cell = &s->cells[r][c];
            if (cell->flags & (CELL_WIDE_CONT | CELL_PAD)) continue;
            n += (size_t)utf8_encode((uint8_t *)buf + n, cell->ch ? cell->ch : ' ');
        }
        if (r < r1 && !wrapped) buf[n++] = '\n';
    }
    *len = n;
    return buf;
}

/* The text we asked to paste, in pieces (see xterm_paste_request).  What
 * is typed to a program must not carry control characters that could
 * act as commands -- an ESC would end a bracketed paste early -- so only
 * tab and newline survive, and newlines go as the CR that Enter sends. */
static void paste_chunk(const uint8_t *d, size_t n, void *unused)
{
    (void)unused;
    if (!paste.send) return;
    if (!d) {                                   /* the end */
        if (paste.started && paste.bracketed) paste.send((const uint8_t *)"\033[201~", 6, paste.ctx);
        paste.started = paste.after_cr = 0;
        return;
    }
    if (!paste.started) {
        paste.started = 1;
        if (paste.bracketed) paste.send((const uint8_t *)"\033[200~", 6, paste.ctx);
    }
    uint8_t out[128]; size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        int skip = b == '\n' && paste.after_cr;      /* the LF of a CR LF */
        paste.after_cr = b == '\r';
        if (b == '\n') b = '\r';
        if (skip || (b < 0x20 && b != '\t' && b != '\r') || b == 0x7f) continue;
        out[k++] = b;
        if (k == sizeof out) { paste.send(out, k, paste.ctx); k = 0; }
    }
    if (k) paste.send(out, k, paste.ctx);
}

/* Ask for the PRIMARY text and send it to the program as it arrives
 * (wrapped as a bracketed paste if it asked for that).  What a right click
 * does, and the paste shortcuts. */
void xscr_paste(int bracketed, SendFn send, void *ctx)
{
    paste.send = send; paste.ctx = ctx; paste.bracketed = (uint8_t)bracketed;
    xterm_paste_request(paste_chunk, NULL);
}

/* Drop the highlighted selection (not while its drag is going, and the
 * copied text stays on the clipboard); returns nonzero if one was shown,
 * so the caller knows to repaint. */
int xscr_selection_drop(void)
{
    if (sel_drag || !sel_on) return 0;
    sel_on = 0;
    return 1;
}

/* The code that consumes pointer events is going away (an observer
 * leaving): pasted text must not be sent to it any more. */
void xscr_pointer_release(void) { paste.send = NULL; }

/* Handle the queued pointer events for the program shown in `s`, which asked
 * for the mouse as mode/enc (see mouse_encode) and for bracketed paste or
 * not, and sees rows x cols cells.  What is for the program is passed to
 * `send(bytes, n, ctx)`, as is pasted text. */
int xscr_pointer(const Screen *s, int mode, int enc, int bracketed,
                 int rows, int cols, SendFn send, void *ctx)
{
    paste.send = send; paste.ctx = ctx; paste.bracketed = (uint8_t)bracketed;
    XtermMouse m;
    uint8_t out[40];
    int redraw = 0, n, wheel = 0;
    while (xterm_read_mouse(&m)) {
        int forced = MOUSE_SELECT_MOD && (m.mods & MOUSE_SELECT_MOD) == MOUSE_SELECT_MOD;
        int pass = 0;

        if (m.button >= 4) {                    /* wheel */
            pass = mode && !forced && grab != 2;
            if (!pass && m.kind == 0 && m.button <= 5)
                wheel += m.button == 4 ? 1 : -1;
        } else if (m.kind == 0 && !grab) {      /* a button goes down */
            if (m.row >= rows || m.col >= cols) continue;   /* not the program's area */
            if (sel_on) { sel_on = 0; redraw = 1; }
            grab = (mode && !forced) ? 1 : 2; grab_btn = m.button;
            if (grab == 2) {
                if (m.button == 1) {
                    sel_a = sel_b = SEL_KEY(m.row, m.col); sel_drag = 1;
                    sel_mode = (m.mods & 8) && !(MOUSE_SELECT_MOD & 8) ? SM_RECT :
                               m.clicks == 2 ? SM_WORD : m.clicks == 3 ? SM_LINE : SM_CHAR;
                    sel_calc(s);                    /* a double or triple click selects now */
                    redraw |= sel_on;
                }
                else if (m.button == 3) xscr_paste(bracketed, send, ctx);
            }
            pass = grab == 1;
        } else if (grab == 2) {                 /* our gesture: motion and the release */
            if (sel_drag && m.kind != 0) {
                int r = m.row < rows ? m.row : rows - 1, c = m.col < cols ? m.col : cols - 1;
                if (SEL_KEY(r, c) != sel_b) { sel_b = SEL_KEY(r, c); sel_calc(s); redraw = 1; }
                if (m.kind == 1 && m.button == 1) {
                    sel_drag = 0;
                    size_t len;
                    char *t = sel_on ? sel_extract(s, &len) : NULL;
                    if (t && len) { xterm_selection_set(t, len); free(sel_buf); sel_buf = t; }
                    else free(t);
                }
            }
        } else {                                /* the program's gesture, or motion with no button */
            pass = grab ? 1 : (m.kind == 2 && mode == 3 && !forced);
        }
        if (m.kind == 1 && grab && m.button == grab_btn) grab = 0;
        if (pass && (n = mouse_encode(&m, mode, enc, rows, cols, out)) > 0)
            send(out, (size_t)n, ctx);
    }
    if (redraw) xscr_render(s);
    return wheel;
}
