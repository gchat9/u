#ifndef XTERM_LIB_H
#define XTERM_LIB_H

uint8_t *xterm_framebuffer(void);
/* Parallel per-cell xterm 256-colour palette indices (0-255), same
 * layout as xterm_framebuffer() (row*columns()+col). Index 15 is this
 * backend's "default fg", index 0 its "default bg" (see xterm_lib.c). */
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
int xterm_wait(int timeout_ms);

/* The underlying X11 connection socket, for callers that want to select()/
 * poll() on it themselves alongside other fds (rather than only calling
 * xterm_wait() on a fixed schedule). */
int xterm_fd(void);

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
