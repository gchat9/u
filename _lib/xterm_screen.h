#pragma once
/*
 * xterm_screen -- show a Screen (vt.h) in the xterm.c window, and handle the
 * pointer for it: the layer between the parser and the X11 window.  See
 * xterm_screen.c.
 */
#include <stddef.h>
#include <stdint.h>

#include "vt.h"
#include "xterm.h"

/* Draws the screen's content and, from the same Screen, keeps the
 * full-block cursor (no blink) current -- see xterm's xterm_set_cursor
 * for why this is folded in here rather than being a separate call. */
void xscr_render(const Screen *screen);

/* Maps a decoded Unicode codepoint to this font's glyph slot (see
 * charset.h): ch==0 (an empty/never-written cell) reads as a blank space.
 * Returns -1 if the charset has no glyph for it.  (Not inline: charset.h's
 * table would be duplicated in every file that used it.) */
int xscr_glyph_slot(uint32_t ch);

/* Glyph slot and attribute for one half of a double-width character (what
 * xscr_render stores for it in the framebuffer and attribute buffers). */
int xscr_wide_glyph(uint32_t wch, int right, uint8_t *attr);

/* Mouse and clipboard.  xterm_mouse_select() says how much pointer traffic
 * is wanted (the displayed program's mouse_mode, 0 none .. 3 all motion;
 * always at least 2 while the mouse is also ours for copy and paste).
 *
 * _pointer handles the queued pointer events for the program shown in s
 * (which asked for the mouse as mode/enc, and bracketed paste or not, and
 * sees rows x cols cells): events for the program, and text pasted, are
 * passed to send(bytes, n, ctx); the rest is text selection and copy, see
 * xterm_screen.c.  _paste pastes PRIMARY (what a right click does);
 * xterm_paste_key() says, once, that Shift+Insert or Ctrl+V asked for that.
 * _selection_drop removes the highlight (nonzero if there was one to
 * repaint); _pointer_release says that send/ctx is going away. */
/* Returns the wheel steps that were not for the program: +1 for each
 * wheel-up press, -1 for each wheel-down press, summed.  A caller with no
 * program to report to (the scrollback viewer) scrolls by them; one that
 * has a program ignores the result. */
int  xscr_pointer(const Screen *s, int mode, int enc, int bracketed,
                  int rows, int cols,
                  void (*send)(const uint8_t *, size_t, void *), void *ctx);
void xscr_paste(int bracketed, void (*send)(const uint8_t *, size_t, void *), void *ctx);
int  xscr_selection_drop(void);
void xscr_pointer_release(void);
