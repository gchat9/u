/*
 * xterm.c — render-only X11 terminal surface
 *
 *   - Pure libc, zero X11 libraries
 *   - Raw X11 wire protocol over Unix socket
 *   - BFNT bitmap font (produced by _font/mkfont.c)
 *   - File-backed BFNT font mapping; protocol buffers supplied by main's stack frame
 *   - Per-cell PutImage (no full-screen framebuffer)
 *   - Dirty-cell tracking (typically 2-3 PutImage calls per keypress)
 *
 * The font is fixed at /etc/font.bfnt.  xterm_init() creates the
 * surface; callers update xterm_framebuffer() and call xterm_render().
 *
 * Assumes: little-endian host; supports 16-bit RGB565 and 32-bpp TrueColor.
 */
#ifdef XTERM_LIB_ONLY
#define EXPORT_IMPLEMENTATIONS 1
#include "../_sys/_.h"
#else
#include "../_sys/_main.h"
#endif

#include "xterm.h"

/* Ugly workaround for xtmux that uses both dietlibc and _sys/ buildsystem */
#ifdef X11_BACKEND
extern char *getenv(const char *name);
#endif

#define AF_UNIX 1

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

/* ── palette (0x00RRGGBB) ─────────────────────────────────────────── */
#define C_BG 0x0D1117u
#define C_FG 0xCDD9E5u
#define C_CU 0x57AB5Au

/* Standard xterm 256-colour palette (see e.g.
 * https://www.ditig.com/256-colors-cheat-sheet), matching the encoding
 * documented in tmux/vt.h for Cell.fg/bg: 0-7 ANSI, 8-15 bright ANSI,
 * 16-231 the 6x6x6 colour cube, 232-255 the greyscale ramp.
 *
 * Slots 0 and 15 (ANSI black / bright white) are overwritten with this
 * backend's own C_BG/C_FG theme colours: CELL_FG_DFL/CELL_BG_DFL (SGR
 * "default colour") are resolved to indices 15/0 rather than needing a
 * separate default-flag byte per cell, so plain unstyled text renders
 * pixel-identical to before this change. The one cost: literal ANSI
 * black (SGR 40) and bright white (SGR 97) become indistinguishable
 * from "default" — a deliberate compactness trade-off. */
static uint32_t palette[256];

static void build_palette(void)
{
    static const uint32_t base16[16] = {
        0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,
        0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff,
    };
    for (int i = 0; i < 16; i++) palette[i] = base16[i];
    static const uint8_t lvl[6] = {0,95,135,175,215,255};
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                palette[16 + r*36 + g*6 + b] =
                    ((uint32_t)lvl[r] << 16) | ((uint32_t)lvl[g] << 8) | lvl[b];
    for (int i = 0; i < 24; i++) {
        uint32_t v = 8 + (uint32_t)i * 10;
        palette[232 + i] = (v << 16) | (v << 8) | v;
    }
    palette[0]  = C_BG;
    palette[15] = C_FG;
}

/* ═══════════════════════════════════════════════════════════════════
 * X11 I/O
 * ═══════════════════════════════════════════════════════════════════ */

static int xfd;

__attribute__((noreturn)) static void die(const char *msg)
{
    write_all_fd(2, msg, strlen(msg));
    exit(1);
    __builtin_unreachable();
}

static void xread(void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        long r = read(xfd, p, n);
        if (r <= 0) die("xread failed\n");
        p += r; n -= r;
    }
}

static void xwrite(const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        long r = write(xfd, p, n);
        if (r <= 0) die("xwrite failed\n");
        p += r; n -= r;
    }
}

static void xdrain(size_t n)
{
    uint8_t tmp[256];
    while (n) {
        size_t c = n < sizeof tmp ? n : sizeof tmp;
        xread(tmp, c); n -= c;
    }
}

static uint32_t xid_base, xid_mask, xid_seq;
/* Supplied by the X11 setup reply; the protocol default is 65535 words. */
static uint32_t x_max_request_words;
static uint32_t new_xid(void) { return xid_base | (xid_seq++ & xid_mask); }

/* ── PutImage (opcode 72, ZPixmap), auto-striped ─────────────────── */
static void put_image(uint32_t draw, uint32_t gc,
                      int dstx, int dsty,
                      int w, int h, uint8_t depth, int bytes_per_pixel,
                      const uint8_t *px)
{
    /* PutImage has a six-word fixed header.  Servers may advertise a
       substantially smaller request limit than the protocol maximum. */
    size_t stride = ((size_t)w * bytes_per_pixel + 3) & ~(size_t)3;
    int max_rows = (int)((x_max_request_words - 6) * 4 / stride);
    if (max_rows < 1) max_rows = 1;
    for (int y0 = 0; y0 < h; y0 += max_rows) {
        int      rows = (y0 + max_rows > h) ? h - y0 : max_rows;
        size_t   dsz  = stride * rows;
        uint16_t rlen = (uint16_t)(6 + dsz / 4);
        uint16_t uw = (uint16_t)w, ur = (uint16_t)rows;
        int16_t  sx = (int16_t)dstx, sy = (int16_t)(dsty + y0);
        uint8_t  hdr[24] = {0};
        hdr[0] = 72; hdr[1] = 2;
        memcpy(hdr+ 2, &rlen, 2); memcpy(hdr+ 4, &draw, 4);
        memcpy(hdr+ 8, &gc,   4); memcpy(hdr+12, &uw,   2);
        memcpy(hdr+14, &ur,   2); memcpy(hdr+16, &sx,   2);
        memcpy(hdr+18, &sy,   2); hdr[21] = depth;
        xwrite(hdr, 24);
        xwrite(px + (size_t)y0 * stride, dsz);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Keyboard
 * ═══════════════════════════════════════════════════════════════════ */

/* GetKeyboardMapping (opcode 101): fixed reply header is 32 bytes, then
 * keysyms_per_keycode * count KEYSYMs (4 bytes each) follow.
 *
 * keysyms_per_keycode is chosen by the server, not us, and grows with
 * every extra shift level or layout group the system has configured
 * (AltGr, a second language group, compose, ...) -- a real desktop's
 * XKB config routinely reports 6-8+ where a bare test server reports
 * 2. This translator only looks at the first four columns of any
 * keycode's row -- the standard 4-level model (base, shift, AltGr,
 * AltGr+shift) that covers every plain accented-letter layout -- so
 * rather than sizing a table for the server's worst case (and
 * silently losing all keyboard input for the rest of the run if some
 * system's config exceeds it -- the previous bug here), stream the
 * reply one keycode-row at a time and keep just those four columns.
 * Table size is then a fixed 256*4 regardless of what the server
 * actually sends. Levels beyond 4 (further layout groups, 5th-level
 * shift, ...) aren't reachable through this translator. */
static uint8_t  min_keycode, max_keycode, keysyms_per_kc;
static uint32_t keysym_table[256][4];

static void load_keyboard_mapping(void)
{
    int count = (int)max_keycode - (int)min_keycode + 1;
    if (count < 1) return;

    uint8_t req[8] = {0};
    uint16_t len = 2;
    req[0] = 101;
    memcpy(req+2, &len, 2);
    req[4] = min_keycode;
    req[5] = (uint8_t)count;
    xwrite(req, 8);

    uint8_t hdr[32];
    xread(hdr, 32);
    if (hdr[0] != 1) return;   /* error reply: leave keysyms_per_kc == 0 */
    uint8_t kspc = hdr[1];
    if (!kspc) return;

    uint8_t row[4 * 32];  /* 32 keysyms/keycode is already far more than
                            * any real config uses; if some server still
                            * exceeds it, drain the rest of that row
                            * rather than losing wire-format sync. */
    size_t rowbytes = (size_t)kspc * 4;
    for (int i = 0; i < count; i++) {
        if (rowbytes <= sizeof row) {
            xread(row, rowbytes);
        } else {
            xread(row, sizeof row);
            xdrain(rowbytes - sizeof row);
        }
        uint32_t v[4] = {0, 0, 0, 0};
        for (int col = 0; col < kspc && col < 4; col++)
            memcpy(&v[col], row + col * 4, 4);
        if (kspc == 1) v[1] = v[0];  /* no distinct shift level: mirror base,
                                       * matching the pre-AltGr behaviour */
        keysym_table[i][0] = v[0]; keysym_table[i][1] = v[1];
        keysym_table[i][2] = v[2]; keysym_table[i][3] = v[3];
    }
    keysyms_per_kc = kspc;
}

/* level: 0=base, 1=shift, 2=AltGr, 3=AltGr+shift. Returns 0 (NoSymbol)
 * if the keymap doesn't define that level for this key -- e.g. any
 * layout with no AltGr configured at all simply never populated
 * columns 2/3, so keysym_for(kc, 2) is already a safe, silent no-op. */
static uint32_t keysym_for(uint8_t keycode, int level)
{
    if (!keysyms_per_kc || keycode < min_keycode || keycode > max_keycode)
        return 0;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    return keysym_table[keycode - min_keycode][level];
}

#define KEY_QUEUE_CAP 256
static uint8_t key_queue[KEY_QUEUE_CAP];
static int key_head, key_tail;

static void key_push(uint8_t b)
{
    int next = (key_tail + 1) % KEY_QUEUE_CAP;
    if (next == key_head) return;   /* queue full: drop, harmless */
    key_queue[key_tail] = b;
    key_tail = next;
}

static void push_str(const char *s) { while (*s) key_push((uint8_t)*s++); }

/* Translate one KeyPress event (32 bytes) into 0+ output bytes. Covers
 * printable Latin-1 (ASCII pushed as-is, 0xA0-0xFF UTF-8-encoded since
 * that's what everything downstream expects on the wire), Ctrl-letter
 * control codes, an Alt meta-prefix, and the common control keys/
 * arrows as ANSI/VT sequences. Anything else is silently ignored.
 *
 * X11's Latin-1 keysyms (0x020-0x0FF) are numerically identical to
 * their Unicode code points, so ks doubles as the code point here with
 * no translation table needed. */
static void handle_keypress(const uint8_t *ev)
{
    uint8_t keycode = ev[1];
    uint16_t state; memcpy(&state, ev+28, 2);
    int shift = (state & 0x0001) != 0;
    int ctrl  = (state & 0x0004) != 0;
    int alt   = (state & 0x0008) != 0;  /* Mod1: conventional "Alt" binding */
    int altgr = (state & 0x0080) != 0;  /* Mod5: conventional AltGr /
                                          * ISO_Level3_Shift binding, as
                                          * produced by e.g. xkb option
                                          * "lv3:ralt_switch" */

    uint32_t ks = keysym_for(keycode, (altgr ? 2 : 0) + (shift ? 1 : 0));
    if (!ks) return;

    /* Meta-prefix: makes Alt+key reach the multiplexer's own shortcuts
     * (e.g. Alt+2 -> ESC '2' -> input.c's FSM_ESC -> CMD_SELECT_WINDOW)
     * exactly like a real terminal's metaSendsEscape.
     *
     * Suppressed when altgr is also set: some real XKB configs leave
     * Right-Alt bound into Mod1 even after lv3:ralt_switch repurposes
     * it as level-3 shift, so a genuine AltGr press can carry both
     * bits. AltGr's intent (an alternate-level character) and Alt's
     * (a meta-prefixed shortcut) are mutually exclusive, and an
     * unwanted ESC here would otherwise get misparsed downstream and
     * silently eat the accented character (see input.c's FSM_ESC,
     * which forwards ESC+one-byte as a pair -- orphaning the second
     * half of what should be a 2-byte UTF-8 sequence). */
    if (alt && !altgr) key_push(0x1B);

    if (ctrl) {
        uint32_t up = ks;
        if (up >= 'a' && up <= 'z') up -= 'a' - 'A';
        if (up >= '@' && up <= '_') { key_push((uint8_t)(up & 0x1f)); return; }
    }

    if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xA0 && ks <= 0xFF)) {
        if (ks <= 0x7F) {
            key_push((uint8_t)ks);
        } else {
            key_push((uint8_t)(0xC0 | (ks >> 6)));
            key_push((uint8_t)(0x80 | (ks & 0x3F)));
        }
        return;
    }

    switch (ks) {
    case 0xFF08: key_push(0x7f); return;   /* BackSpace */
    case 0xFF09: key_push('\t');  return;   /* Tab */
    case 0xFF0D: key_push('\r');  return;   /* Return */
    case 0xFF1B: key_push(0x1b); return;   /* Escape */
    /* The rest are all ANSI/VT escape sequences -- exactly what the
     * child's TERM=xterm-256color terminfo (see pty.c) expects, so
     * readline/ncurses there recognize them the same way a real xterm's
     * keys would be recognized. */
    case 0xFF51: push_str("\x1b[D");   return;  /* Left */
    case 0xFF52: push_str("\x1b[A");   return;  /* Up */
    case 0xFF53: push_str("\x1b[C");   return;  /* Right */
    case 0xFF54: push_str("\x1b[B");   return;  /* Down */
    case 0xFF50: push_str("\x1bOH");   return;  /* Home */
    case 0xFF57: push_str("\x1bOF");   return;  /* End */
    case 0xFF55: push_str("\x1b[5~");  return;  /* Page_Up */
    case 0xFF56: push_str("\x1b[6~");  return;  /* Page_Down */
    case 0xFF63: push_str("\x1b[2~");  return;  /* Insert */
    case 0xFFFF: push_str("\x1b[3~");  return;  /* Delete */
    case 0xFFBE: push_str("\x1bOP");   return;  /* F1  */
    case 0xFFBF: push_str("\x1bOQ");   return;  /* F2  */
    case 0xFFC0: push_str("\x1bOR");   return;  /* F3  */
    case 0xFFC1: push_str("\x1bOS");   return;  /* F4  */
    case 0xFFC2: push_str("\x1b[15~"); return;  /* F5  */
    case 0xFFC3: push_str("\x1b[17~"); return;  /* F6  */
    case 0xFFC4: push_str("\x1b[18~"); return;  /* F7  */
    case 0xFFC5: push_str("\x1b[19~"); return;  /* F8  */
    case 0xFFC6: push_str("\x1b[20~"); return;  /* F9  */
    case 0xFFC7: push_str("\x1b[21~"); return;  /* F10 */
    case 0xFFC8: push_str("\x1b[23~"); return;  /* F11 */
    case 0xFFC9: push_str("\x1b[24~"); return;  /* F12 */
    default: return;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * BFNT font
 * ═══════════════════════════════════════════════════════════════════ */

#define FONT_MAX       (128 * 1024)
#define GRID_MAX_CELLS (32 * 1024)
#define SETUP_MAX      (8 * 1024)

static uint8_t  *mmap_base;
static size_t    mmap_size;
static uint16_t  cell_w, cell_h, baseline, first_cp, num_glyphs;
static uint8_t  *glyph_data;
static int       WIN_W, WIN_H, cols, rows;

static void font_load(const char *path)
{
    int fd = (int)open(path, O_RDONLY, 0);
    if (fd < 0) die("could not open font\n");
    mmap_base = mmap(0, FONT_MAX, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mmap_base == MAP_FAILED) die("could not map font\n");
    mmap_size = FONT_MAX;
    if (mmap_size < 16 || mmap_base[0] != 'B' || mmap_base[1] != 'F' ||
        mmap_base[2] != 'N' || mmap_base[3] != 'T') {
        die("invalid BFNT font\n");
    }
    memcpy(&cell_w,     mmap_base+ 4, 2);
    memcpy(&cell_h,     mmap_base+ 6, 2);
    memcpy(&baseline,   mmap_base+ 8, 2);
    memcpy(&first_cp,   mmap_base+10, 2);
    memcpy(&num_glyphs, mmap_base+12, 2);
    size_t glyph_bytes = (size_t)cell_w * cell_h * num_glyphs;
    if (glyph_bytes > FONT_MAX - 16) die("font is too large\n");
    glyph_data = mmap_base + 16;
}

/* emoji.bfnt (see _font/mkemoji.c): optional -- if it's missing or doesn't
 * match the font's cell size, every emoji draws as a placeholder box
 * (see draw_box) rather than killing the terminal. */
#define EMOJI_MAX (32 * 1024)
static const uint8_t *emoji_data;   /* NULL = no emoji available */
static uint16_t emoji_count;
static size_t   emoji_rec;          /* bytes per record: 45 palette + packed 4bpp pixels */

static void emoji_load(const char *path)
{
    int fd = (int)open(path, O_RDONLY, 0);
    if (fd < 0) return;
    uint8_t *m = mmap(0, EMOJI_MAX, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    uint16_t w, h, n;
    memcpy(&w, m + 4, 2); memcpy(&h, m + 6, 2); memcpy(&n, m + 8, 2);
    size_t rec = 45 + ((size_t)w * 2 * h + 1) / 2;
    if (m[0] != 'E' || m[1] != 'M' || m[2] != 'O' || m[3] != 'J' ||
        w != cell_w || h != cell_h || 12 + (size_t)n * rec > EMOJI_MAX)
        return;
    emoji_data = m; emoji_count = n; emoji_rec = rec;
}

/* ═══════════════════════════════════════════════════════════════════
 * Terminal state
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t *scr;
static uint8_t *fgb, *bgb, *atb;
static uint8_t fg_storage[GRID_MAX_CELLS];   /* xterm 256-colour index per cell */
static uint8_t bg_storage[GRID_MAX_CELLS];
static uint8_t attr_storage[GRID_MAX_CELLS]; /* XTERM_ATTR_* bitmask per cell    */

static void grid_init(uint16_t screen_w, uint16_t screen_h,
                      uint8_t *screen, size_t capacity)
{
    cols = screen_w / cell_w;
    rows = screen_h / cell_h;
    if (cols < 1 || rows < 1) {
        die("screen is smaller than one font cell\n");
    }
    WIN_W = cols * cell_w;
    WIN_H = rows * cell_h;
    size_t cells = (size_t)cols * rows;
    if (cells > capacity) die("terminal grid is too large\n");
    scr = screen;
    memset(scr, 0, cells);
    fgb = fg_storage; bgb = bg_storage; atb = attr_storage;
    memset(fgb, 15, cells);  /* default fg (palette[15], see build_palette) */
    memset(bgb, 0,  cells);  /* default bg (palette[0])                     */
    memset(atb, 0,  cells);
}

/* ═══════════════════════════════════════════════════════════════════
 * Per-cell rendering
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t *cell_buf;

static inline uint32_t blend(uint8_t a, uint32_t src, uint32_t dst)
{
    if (a==  0) return dst;
    if (a==255) return src;
    unsigned sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    unsigned dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    return (uint32_t)(((sr*a+dr*(255-a))/255)<<16|
                      ((sg*a+dg*(255-a))/255)<< 8|
                      ((sb*a+db*(255-a))/255));
}

static inline uint16_t pack565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

/* Solid horizontal line, full cell width, at cell-local row `r` (0 = top
 * of cell). Used for ATTR_UNDERLINE / ATTR_STRIKE — cheap structural
 * marks that a colour change alone can't express. */
static void fill_row(int r, uint32_t color, uint8_t depth)
{
    if (r < 0 || r >= cell_h) return;
    if (depth == 16) {
        size_t stride = ((size_t)cell_w * 2 + 3) & ~(size_t)3;
        uint16_t *line = (uint16_t *)(cell_buf + (size_t)r * stride);
        uint16_t p = pack565(color);
        for (int x = 0; x < cell_w; x++) line[x] = p;
    } else {
        uint32_t *line = (uint32_t *)cell_buf + (size_t)r * cell_w;
        for (int x = 0; x < cell_w; x++) line[x] = color;
    }
}

/* Store one pixel (i = y*cell_w + x) into cell_buf at the window depth. */
static inline void put_px(int i, uint32_t color, uint8_t depth)
{
    if (depth == 16) {
        size_t stride = ((size_t)cell_w * 2 + 3) & ~(size_t)3;
        uint16_t *line = (uint16_t *)(cell_buf + (size_t)(i / cell_w) * stride);
        line[i % cell_w] = pack565(color);
    } else {
        ((uint32_t *)cell_buf)[i] = color;
    }
}

/* Draw the left (half=0) or right (half=1) half of emoji k over the
 * already-bg-filled cell_buf. Each record is 15 RGB palette entries
 * (index 0 = transparent, implicit) then 4-bit indices, row-major over
 * the full two-cell-wide bitmap, first pixel in the high nibble. */
/* Placeholder for a character we have no glyph for: a 1px outline in
 * the cell's foreground colour, inset 1px from the edge of a w-pixel-
 * wide glyph area, of which this cell shows the columns starting at
 * pixel `off` -- so a double-width emoji (w = 2 cells) draws its left
 * and right halves as two pieces of one rectangle, and an ordinary
 * cell (off = 0, w = 1 cell) draws the whole thing. Drawn rather than
 * stored, so it needs no data (and works with no emoji.bfnt at all)
 * and follows fg colour, including the cursor's inversion. */
static void draw_box(int off, int w, uint32_t fg, uint8_t depth)
{
    int last = w - 2;                          /* rightmost line column */
    for (int y = 1; y < cell_h - 1; y++)
        for (int x = 0; x < cell_w; x++) {
            int X = off + x;
            if (X >= 1 && X <= last && (X == 1 || X == last || y == 1 || y == cell_h - 2))
                put_px(y * cell_w + x, fg, depth);
        }
}

static void draw_emoji(int k, int half, uint32_t fg, uint8_t depth)
{
    if (!emoji_data || k >= emoji_count) { draw_box(half * cell_w, cell_w * 2, fg, depth); return; }
    const uint8_t *rec = emoji_data + 12 + (size_t)k * emoji_rec;
    const uint8_t *pix = rec + 45;
    int gw = cell_w * 2;
    for (int y = 0; y < cell_h; y++)
        for (int x = 0; x < cell_w; x++) {
            int p  = y * gw + half * cell_w + x;
            int ix = (p & 1) ? pix[p >> 1] & 15 : pix[p >> 1] >> 4;
            if (!ix) continue;
            const uint8_t *c = rec + (ix - 1) * 3;
            put_px(y * cell_w + x,
                   (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2], depth);
        }
}

/* Cursor: a plain full-block inversion (fg/bg swapped for the whole
 * cell), no blinking. row<0 means "no cursor" (hidden/unfocused etc). */
static int cursor_row = -1, cursor_col = -1;

static void render_cell(int row, int col,
                        uint32_t draw, uint32_t gc, uint8_t depth)
{
    int idx = row * cols + col;
    uint8_t rattrs = atb[idx];
    /* A wide character (emoji or fullwidth form) is one character over
     * two cells: the cursor inverts both halves whichever of the two
     * it's on, never just half. */
    int half = (rattrs & (XTERM_ATTR_EMOJI_R | XTERM_ATTR_FW_R)) != 0;
    int pair = col - half;
    int is_emoji = (rattrs & (XTERM_ATTR_EMOJI_L | XTERM_ATTR_EMOJI_R)) != 0;
    int is_fw = (rattrs & (XTERM_ATTR_FW_L | XTERM_ATTR_FW_R)) != 0;
    int is_cursor = row == cursor_row &&
        (is_emoji || is_fw ? (cursor_col == pair || cursor_col == pair + 1)
                           : cursor_col == col);
    uint32_t bg = palette[bgb[idx]];
    uint32_t fg = palette[fgb[idx]];
    if (is_cursor) { uint32_t t = bg; bg = fg; fg = t; }
    int n = cell_w * cell_h;
    if (depth == 16) {
        size_t stride = ((size_t)cell_w * 2 + 3) & ~(size_t)3;
        for (int y = 0; y < cell_h; y++) {
            uint16_t *line = (uint16_t *)(cell_buf + (size_t)y * stride);
            for (int x = 0; x < cell_w; x++) line[x] =
                (uint16_t)(((bg >> 8) & 0xf800) | ((bg >> 5) & 0x07e0) |
                           ((bg >> 3) & 0x001f));
        }
    } else {
        uint32_t *line = (uint32_t *)cell_buf;
        for (int i = 0; i < n; i++) line[i] = bg;
    }
    uint8_t cp = scr[idx];
    if (is_emoji) {
        draw_emoji(cp, half, fg, depth);
    } else if (rattrs & XTERM_ATTR_MISSING) {
        draw_box(0, cell_w, fg, depth);
    } else if (cp >= first_cp && cp < (uint16_t)(first_cp + num_glyphs)) {
        const uint8_t *g = glyph_data + (size_t)(cp-first_cp)*cell_w*cell_h;
        /* A fullwidth character reuses its narrow twin's glyph, centred
         * across the two cells; this cell shows glyph columns x - sh
         * (left half: shifted right by half a cell; right half: the
         * remainder). Ordinary cells: sh = 0, every column in range. */
        int sh = is_fw ? cell_w / 2 - half * cell_w : 0;
        for (int y = 0; y < cell_h; y++)
            for (int x = 0; x < cell_w; x++) {
                int gx = x - sh;
                uint8_t a = gx >= 0 && gx < cell_w ? g[y * cell_w + gx] : 0;
                if (a) put_px(y * cell_w + x, blend(a, fg, bg), depth);
            }
    }
    if (rattrs & XTERM_ATTR_UNDERLINE) {
        int r = baseline + 1;
        fill_row(r >= cell_h ? cell_h - 1 : r, fg, depth);
    }
    if (rattrs & XTERM_ATTR_STRIKE) {
        fill_row(baseline / 2, fg, depth);
    }
    put_image(draw, gc, col*cell_w, row*cell_h, cell_w, cell_h, depth,
              depth == 16 ? 2 : 4, cell_buf);
}

static void render_all(uint32_t draw, uint32_t gc, uint8_t depth)
{
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            render_cell(r, c, draw, gc, depth);
        }
    }
}


/* Library-owned state and backing storage. */
static uint8_t screen_storage[GRID_MAX_CELLS];
static uint8_t cell_storage[64*64*4];
static uint32_t xterm_win, xterm_gc;

/* ═══════════════════════════════════════════════════════════════════
 * Atoms, window title, close protocol
 * ═══════════════════════════════════════════════════════════════════ */

/* Synchronous InternAtom (opcode 16): resolve a name to its ATOM id.
 *
 * By the time this is called the window already exists (it's used for
 * the WM_PROTOCOLS/WM_DELETE_WINDOW setup, right after CreateWindow +
 * MapWindow + grab_focus's SetInputFocus), so ordinary events -- an
 * EnterNotify or Expose triggered by any of that -- can legitimately be
 * sitting in front of our reply on the wire. A reply is always byte0==1
 * and an error is byte0==0; anything else is an event that arrived
 * first and simply isn't relevant here (xterm_wait's own loop is what
 * processes events, once the main loop starts), so skip over it rather
 * than misreading its bytes as if they were our reply. */
static uint32_t intern_atom(const char *name, int len)
{
    uint8_t req[24] = {0};
    uint16_t reqlen = (uint16_t)(2 + ((len + 3) / 4));
    uint16_t nlen = (uint16_t)len;
    req[0] = 16; req[1] = 0; /* only-if-exists = 0 (create it) */
    memcpy(req+2, &reqlen, 2);
    memcpy(req+4, &nlen, 2);
    memcpy(req+8, name, (size_t)len);
    xwrite(req, (size_t)reqlen * 4);
    for (;;) {
        uint8_t rep[32]; xread(rep, 32);
        if (rep[0] == 1) { uint32_t atom; memcpy(&atom, rep+8, 4); return atom; }
        if (rep[0] == 0) return 0; /* error: give up gracefully */
        /* else: an unrelated event, drop it and keep waiting */
    }
}

static uint32_t wm_protocols_atom, wm_delete_window_atom;
static int close_requested;

/* WM_NAME (STRING, format 8) — bare-minimum window title. */
void xterm_set_title(const char *name, int len)
{
    uint8_t buf[24 + 256];
    if ((size_t)len > sizeof buf - 24) len = sizeof buf - 24; /* generous cap */
    uint16_t reqlen = (uint16_t)(6 + ((len + 3) / 4));
    uint32_t nlen = (uint32_t)len;
    uint8_t req[24] = {0};
    req[0] = 18; req[1] = 0; /* ChangeProperty, mode=Replace */
    memcpy(req+2, &reqlen, 2);
    memcpy(req+4, &xterm_win, 4);
    uint32_t prop = 39, type = 31; /* WM_NAME, STRING: predefined atoms */
    memcpy(req+8, &prop, 4);
    memcpy(req+12, &type, 4);
    req[16] = 8; /* format */
    memcpy(req+20, &nlen, 4);
    memcpy(buf, req, 24);
    memcpy(buf + 24, name, (size_t)len);
    xwrite(buf, (size_t)reqlen * 4);
}

int xterm_close_requested(void) { return close_requested; }
static uint8_t xterm_depth;

uint8_t *xterm_framebuffer(void) { return scr; }
/* Parallel to xterm_framebuffer(): one xterm 256-colour palette index
 * per cell. Defaults (palette[15]/palette[0], see build_palette) fill
 * cells the caller never touches. */
uint8_t *xterm_fg_buffer(void) { return fgb; }
uint8_t *xterm_bg_buffer(void) { return bgb; }
uint8_t *xterm_attr_buffer(void) { return atb; }
int xterm_columns(void) { return cols; }
int xterm_rows(void) { return rows; }

/* Claim keyboard focus explicitly (revert to PointerRoot if the window
 * ever goes away). Without this, key delivery depends entirely on
 * whatever focus policy the server/WM (if any) happens to be running,
 * which is fragile — e.g. under a bare Xvnc with no window manager, or
 * under a real WM whose own focus-on-map policy might race ours. Called
 * once at startup and again on every EnterNotify so focus reliably
 * follows the pointer into the window regardless of WM policy. */
static void grab_focus(void)
{
    uint8_t foc[12] = {0}; uint16_t foc_len=3; uint32_t time0=0;
    foc[0]=42; foc[1]=1; /* revert-to = PointerRoot */
    memcpy(foc+2,&foc_len,2); memcpy(foc+4,&xterm_win,4); memcpy(foc+8,&time0,4);
    xwrite(foc,12);
}

int xterm_init(void)
{
    uint8_t setup_storage[SETUP_MAX];
    x_max_request_words = 65535;
    cell_buf = cell_storage;
    build_palette();
    font_load("/etc/font.bfnt");
    emoji_load("/etc/emoji.bfnt");

    xfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, "/tmp/.X11-unix/X", sizeof sa.sun_path);
    /* $DISPLAY is ":N" or ":NN" (optionally ".screen", which we don't
     * need -- only the display number selects the socket). Assume N is
     * 0-99 (true of any real X display) and copy its 1-2 digit
     * characters straight out of the environment string; there's
     * nothing to compute, so no atoi()/snprintf() round-trip. Falls
     * back to display 2 (the old hardcoded default) if $DISPLAY is
     * unset or doesn't look like ":<digits>". */
    size_t plen = strlen(sa.sun_path);
#ifdef X11_BACKEND
    const char *disp = getenv("DISPLAY");
#else
    const char *disp = NULL;  /* standalone demo: no libc, no $DISPLAY */
#endif
    if (disp && disp[0] == ':' && disp[1] >= '0' && disp[1] <= '9') {
        sa.sun_path[plen++] = disp[1];
        if (disp[2] >= '0' && disp[2] <= '9') sa.sun_path[plen++] = disp[2];
        sa.sun_path[plen] = '\0';
    } else {
        sa.sun_path[plen]   = '2';
        sa.sun_path[plen+1] = '\0';
    }
    int sa_len = (int)(sizeof sa.sun_family + strlen(sa.sun_path) + 1);
    if (connect(xfd, (struct sockaddr *)&sa, sa_len) < 0)
        die("could not connect to X11\n");

    uint8_t sreq[12] = { 'l',0, 11,0, 0,0, 0,0, 0,0, 0,0 };
    xwrite(sreq, 12);
    uint8_t shdr[8]; xread(shdr, 8);
    if (shdr[0] != 1) die("X11 setup failed\n");
    uint16_t alen; memcpy(&alen, shdr+6, 2);
    size_t setup_size = (size_t)alen * 4;
    if (setup_size > sizeof setup_storage) die("X11 setup reply is too large\n");
    uint8_t *d = setup_storage; xread(d, setup_size);
    memcpy(&xid_base, d+4, 4); memcpy(&xid_mask, d+8, 4);
    uint16_t vlen; memcpy(&vlen, d+16, 2);
    uint16_t max_request_words; memcpy(&max_request_words, d+18, 2);
    if (max_request_words > 6) x_max_request_words = max_request_words;
    uint8_t nfmt = d[21];
    size_t soff = 32 + ((vlen+3u)&~3u) + (size_t)nfmt*8;
    uint32_t root_win; memcpy(&root_win, d+soff, 4);
    uint32_t black_px; memcpy(&black_px, d+soff+12, 4);
    uint16_t screen_w; memcpy(&screen_w, d+soff+20, 2);
    uint16_t screen_h; memcpy(&screen_h, d+soff+22, 2);
    xterm_depth = d[soff+38];
    min_keycode = d[26];
    max_keycode = d[27];
    load_keyboard_mapping();
    grid_init(screen_w, screen_h, screen_storage, sizeof screen_storage);

    xterm_win = new_xid(); xterm_gc = new_xid();
    uint8_t r[40] = {0};
    uint16_t len=10;
    int16_t x=(int16_t)((screen_w-WIN_W)/2), y=(int16_t)((screen_h-WIN_H)/2);
    uint16_t w=(uint16_t)WIN_W, h=(uint16_t)WIN_H, bw=0, cls=1;
    uint32_t vis=0, vmask=(1u<<1)|(1u<<11), bg=black_px;
    uint32_t emask=(1u<<15)|(1u<<0)|(1u<<4); /* Exposure|KeyPress|EnterWindow */
    r[0]=1;
    memcpy(r+2,&len,2); memcpy(r+4,&xterm_win,4); memcpy(r+8,&root_win,4);
    memcpy(r+12,&x,2); memcpy(r+14,&y,2); memcpy(r+16,&w,2); memcpy(r+18,&h,2);
    memcpy(r+20,&bw,2); memcpy(r+22,&cls,2); memcpy(r+24,&vis,4);
    memcpy(r+28,&vmask,4); memcpy(r+32,&bg,4); memcpy(r+36,&emask,4);
    xwrite(r, 40);
    uint8_t map[8] = {0}; uint16_t map_len=2; map[0]=8;
    memcpy(map+2,&map_len,2); memcpy(map+4,&xterm_win,4); xwrite(map,8);
    uint8_t gc[16] = {0}; uint16_t gc_len=4; uint32_t mask=0; gc[0]=55;
    memcpy(gc+2,&gc_len,2); memcpy(gc+4,&xterm_gc,4); memcpy(gc+8,&xterm_win,4);
    memcpy(gc+12,&mask,4); xwrite(gc,16);

    grab_focus();

    /* Opt into the WM_DELETE_WINDOW handshake so a window-manager-driven
     * close (clicking the window's close button, Alt-F4, etc) arrives as
     * an ordinary ClientMessage event instead of the connection just
     * dying under us. */
    wm_protocols_atom     = intern_atom("WM_PROTOCOLS", 12);
    wm_delete_window_atom = intern_atom("WM_DELETE_WINDOW", 16);
    uint8_t wp[28] = {0}; uint16_t wp_len=7; uint32_t wp_n=1;
    wp[0]=18; /* ChangeProperty, mode=Replace */
    memcpy(wp+2,&wp_len,2); memcpy(wp+4,&xterm_win,4);
    memcpy(wp+8,&wm_protocols_atom,4);
    uint32_t atom_type=4; memcpy(wp+12,&atom_type,4); /* type=ATOM */
    wp[16]=32; /* format */
    memcpy(wp+20,&wp_n,4);
    memcpy(wp+24,&wm_delete_window_atom,4);
    xwrite(wp,28);

    return 0;
}

int xterm_fd(void) { return xfd; }

int xterm_read_key(uint8_t *buf, int cap)
{
    int n = 0;
    while (n < cap && key_head != key_tail) {
        buf[n++] = key_queue[key_head];
        key_head = (key_head + 1) % KEY_QUEUE_CAP;
    }
    return n;
}

void xterm_render(void)
{
    render_all(xterm_win, xterm_gc, xterm_depth);
}

/* row<0 hides the cursor; otherwise it's shown at (row,col) as a full-block
 * fg/bg inversion on the next xterm_render() call. No blinking. */
void xterm_set_cursor(int row, int col)
{
    cursor_row = row;
    cursor_col = col;
}

/* Wait for X events, returning nonzero when an expose series is complete. */
int xterm_wait(int timeout_ms)
{
    struct pollfd p = { .fd = xfd, .events = POLLIN, .revents = 0 };
    long ready = poll(&p, 1, timeout_ms);
    if (ready <= 0) return 0;

    int redraw = 0;
    for (;;) {
        if (!(p.revents & POLLIN)) break;
        uint8_t ev[32];
        xread(ev, sizeof ev);
        uint8_t etype = ev[0] & 0x7f;
        if (etype == 12) {
            uint16_t count;
            memcpy(&count, ev + 16, 2);
            if (count == 0) redraw = 1;
        } else if (etype == 2) {
            handle_keypress(ev);
        } else if (etype == 7) {
            /* EnterNotify: pointer entered the window. Re-assert focus so
             * typing works regardless of the WM's own focus policy. */
            grab_focus();
        } else if (etype == 33) {
            /* ClientMessage: check for a WM_DELETE_WINDOW close request. */
            uint32_t mtype; memcpy(&mtype, ev + 8, 4);
            if (mtype == wm_protocols_atom) {
                uint32_t data0; memcpy(&data0, ev + 12, 4);
                if (data0 == wm_delete_window_atom) close_requested = 1;
            }
        }
        p.revents = 0;
        ready = poll(&p, 1, 0);
        if (ready <= 0) break;
    }
    return redraw;
}
