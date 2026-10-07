#ifndef _LIB_XTERM_H
#define _LIB_XTERM_H

uint8_t *xterm_framebuffer(void);
/* Parallel per-cell xterm 256-colour palette indices (0-255), same
 * layout as xterm_framebuffer() (row*columns()+col). Index 15 is this
 * backend's "default fg", index 0 its "default bg" (see xterm.c). */
uint8_t *xterm_fg_buffer(void);
uint8_t *xterm_bg_buffer(void);

/* Parallel per-cell structural-attribute bitmask (only attrs that need
 * extra pixels drawn, not just a colour swap, live here). Independent
 * of tmux/vt.h's ATTR_* bit values by design — callers translate. */
#define XTERM_ATTR_UNDERLINE 0x01
#define XTERM_ATTR_STRIKE    0x02
/* Emoji occupy two adjacent cells (see emoji.bfnt / emoji_charset.h).
 * For a cell flagged EMOJI_L or EMOJI_R, the byte in xterm_framebuffer()
 * is the emoji's *index* (not a glyph slot), and the cell draws the left
 * or right half of that emoji's colour bitmap; transparent pixels show
 * the cell's bg colour. Both halves of a pair must be flagged. */
#define XTERM_ATTR_EMOJI_L   0x04
#define XTERM_ATTR_EMOJI_R   0x08
/* The glyph byte for this (single-width) cell has no meaning: the font
 * has nothing for the character, so draw a placeholder outline box in
 * the cell's fg colour instead of any glyph. */
#define XTERM_ATTR_MISSING   0x10
/* Like EMOJI_L/R, but the glyph byte is an ordinary glyph slot: a
 * double-width character drawn by centring that one narrow glyph across
 * the two cells (fullwidth punctuation, see charset.h). */
#define XTERM_ATTR_FW_L      0x20
#define XTERM_ATTR_FW_R      0x40
uint8_t *xterm_attr_buffer(void);
int xterm_columns(void);
int xterm_rows(void);
int xterm_init(void);
void xterm_render(void);
/* Process pending X events, waiting up to timeout_ms for the first.
 * Returns nonzero if the window's contents must be repainted: it was
 * exposed, or it was resized.  After a resize xterm_columns() and
 * xterm_rows() return the new grid size and all per-cell buffers have
 * been reset (their row stride is the column count), so callers must
 * refill them in full, from scratch, before the next xterm_render().
 * The grid is the window size in whole cells, at least 1x1, and capped
 * at 32K cells in total. */
int xterm_wait(int timeout_ms);

/* The underlying X11 connection socket, for callers that want to select()/
 * poll() on it themselves alongside other fds (rather than only calling
 * xterm_wait() on a fixed schedule). */
int xterm_fd(void);

/* Pointer events.  xterm_mouse_select() says which the application wants
 * (0 none, 1 buttons, 2 buttons and motion while one is held, 3 all
 * motion); only those are requested from the server.  xterm_read_mouse()
 * returns the queued events one at a time (nonzero if it filled *m).
 * Positions are grid cells, clamped to the grid.  Motion is reported only
 * when the pointer enters another cell. */
typedef struct {
    uint8_t  kind;     /* 0 press, 1 release, 2 motion                     */
    uint8_t  button;   /* 1 left, 2 middle, 3 right, 4-7 wheel up/down/
                          left/right; for motion, the button held (0 none) */
    uint8_t  mods;     /* xterm's bits: 4 shift, 8 alt, 16 control         */
    uint16_t col, row;
} XtermMouse;

void xterm_mouse_select(int level);
int  xterm_read_mouse(XtermMouse *m);

/* Nonzero once the connection to the server has been lost (see xterm.c);
 * the window is gone and every further call is a harmless no-op. */
int xterm_lost(void);

/* Close the connection (the server destroys the window).  Call before
 * fork()ing a process that must not keep the window alive. */
void xterm_disconnect(void);

/* Drain up to `cap` bytes of keyboard input translated from KeyPress events
 * (plain ASCII for printable keys, Ctrl-letter control codes, and a handful
 * of ANSI escape sequences for arrows/backspace/etc) into `buf`. Events are
 * queued as a side effect of xterm_wait(); call this after xterm_wait()
 * reports activity to retrieve what it queued. Returns the number of bytes
 * written (0 if none pending). */
int xterm_read_key(uint8_t *buf, int cap);

/* Cursor: full-block fg/bg inversion at (row,col) on the next
 * xterm_render(). No blinking. Pass row<0 to hide it. */
void xterm_set_cursor(int row, int col);

/* Set WM_NAME (window title / taskbar label). */
void xterm_set_title(const char *name, int len);

/* Nonzero once the window manager has sent a WM_DELETE_WINDOW request
 * (the user closed the window). Sticky — stays set once seen. */
int xterm_close_requested(void);

#endif
