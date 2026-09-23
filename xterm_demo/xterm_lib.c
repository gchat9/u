/*
 * xterm_lib.c — render-only X11 terminal surface
 *
 *   - Pure libc, zero X11 libraries
 *   - Raw X11 wire protocol over Unix socket
 *   - BFNT bitmap font (produced by mkfont.c)
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

#define AF_UNIX 1

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

/* ── palette (0x00RRGGBB) ─────────────────────────────────────────── */
#define C_BG 0x0D1117u
#define C_FG 0xCDD9E5u
#define C_CU 0x57AB5Au

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
 * keysyms_per_keycode * count KEYSYMs (4 bytes each) follow. */
#define KEYSYM_TABLE_MAX (256 * 8)

static uint8_t  min_keycode, max_keycode, keysyms_per_kc;
static uint32_t keysym_table[KEYSYM_TABLE_MAX];

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
    uint32_t words; memcpy(&words, hdr+4, 4);
    size_t n = (size_t)words;  /* count * keysyms-per-keycode, in words */
    if (n > KEYSYM_TABLE_MAX) { xdrain(n * 4); return; }
    xread(keysym_table, n * 4);
    keysyms_per_kc = hdr[1];
}

static uint32_t keysym_for(uint8_t keycode, int shifted)
{
    if (!keysyms_per_kc || keycode < min_keycode || keycode > max_keycode)
        return 0;
    int idx = (keycode - min_keycode) * keysyms_per_kc +
              (shifted && keysyms_per_kc > 1 ? 1 : 0);
    return keysym_table[idx];
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

/* Translate one KeyPress event (32 bytes) into 0+ output bytes. Covers
 * printable Latin-1, Ctrl-letter control codes, and the common control
 * keys/arrows as ANSI/VT sequences. Anything else is silently ignored. */
static void handle_keypress(const uint8_t *ev)
{
    uint8_t keycode = ev[1];
    uint16_t state; memcpy(&state, ev+28, 2);
    int shift = (state & 0x0001) != 0;
    int ctrl  = (state & 0x0004) != 0;

    uint32_t ks = keysym_for(keycode, shift);
    if (!ks) return;

    if (ctrl) {
        uint32_t up = ks;
        if (up >= 'a' && up <= 'z') up -= 'a' - 'A';
        if (up >= '@' && up <= '_') { key_push((uint8_t)(up & 0x1f)); return; }
    }

    if (ks >= 0x20 && ks <= 0x7e) { key_push((uint8_t)ks); return; }

    switch (ks) {
    case 0xFF08: key_push(0x7f); return;                              /* BackSpace */
    case 0xFF09: key_push('\t'); return;                              /* Tab */
    case 0xFF0D: key_push('\r'); return;                              /* Return */
    case 0xFF1B: key_push(0x1b); return;                              /* Escape */
    case 0xFF51: key_push(0x1b); key_push('['); key_push('D'); return; /* Left */
    case 0xFF52: key_push(0x1b); key_push('['); key_push('A'); return; /* Up */
    case 0xFF53: key_push(0x1b); key_push('['); key_push('C'); return; /* Right */
    case 0xFF54: key_push(0x1b); key_push('['); key_push('B'); return; /* Down */
    case 0xFFFF: key_push(0x1b); key_push('['); key_push('3'); key_push('~'); return; /* Delete */
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

/* ═══════════════════════════════════════════════════════════════════
 * Terminal state
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t *scr;

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
}

/* ═══════════════════════════════════════════════════════════════════
 * Per-cell rendering
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t *cell_buf;

static inline uint32_t blend(uint8_t a, uint32_t src, uint32_t dst)
{
    if (!a) return dst; if (a==255) return src;
    unsigned sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    unsigned dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    return (uint32_t)(((sr*a+dr*(255-a))/255)<<16|
                      ((sg*a+dg*(255-a))/255)<< 8|
                      ((sb*a+db*(255-a))/255));
}

static void render_cell(int row, int col,
                        uint32_t draw, uint32_t gc, uint8_t depth)
{
    uint32_t bg = C_BG;
    uint32_t fg = C_FG;
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
    uint8_t cp = scr[row*cols + col];
    if (cp >= 0x20 && cp >= first_cp && cp < (uint16_t)(first_cp + num_glyphs)) {
        const uint8_t *g = glyph_data + (size_t)(cp-first_cp)*cell_w*cell_h;
        for (int i = 0; i < n; i++) {
            uint8_t a = g[i];
            if (!a) continue;
            uint32_t color = blend(a, fg, bg);
            if (depth == 16) {
                size_t stride = ((size_t)cell_w * 2 + 3) & ~(size_t)3;
                uint16_t *line = (uint16_t *)(cell_buf + (size_t)(i / cell_w) * stride);
                line[i % cell_w] = (uint16_t)(((color >> 8) & 0xf800) |
                                                ((color >> 5) & 0x07e0) |
                                                ((color >> 3) & 0x001f));
            } else {
                ((uint32_t *)cell_buf)[i] = color;
            }
        }
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
static uint8_t xterm_depth;

uint8_t *xterm_framebuffer(void) { return scr; }
int xterm_columns(void) { return cols; }
int xterm_rows(void) { return rows; }

int xterm_init(void)
{
    uint8_t setup_storage[SETUP_MAX];
    x_max_request_words = 65535;
    cell_buf = cell_storage;
    font_load("/etc/font.bfnt");

    xfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, "/tmp/.X11-unix/X2", sizeof sa.sun_path);
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
    uint32_t emask=(1u<<15)|(1u<<0);
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

    /* Claim keyboard focus explicitly (revert to PointerRoot if the window
     * ever goes away). Without this, key delivery depends entirely on
     * whatever focus policy the server/WM (if any) happens to be running,
     * which is fragile — e.g. under a bare Xvnc with no window manager,
     * focus may or may not already be tracking the pointer. */
    uint8_t foc[12] = {0}; uint16_t foc_len=3; uint32_t time0=0;
    foc[0]=42; foc[1]=1; /* revert-to = PointerRoot */
    memcpy(foc+2,&foc_len,2); memcpy(foc+4,&xterm_win,4); memcpy(foc+8,&time0,4);
    xwrite(foc,12);

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
        }
        p.revents = 0;
        ready = poll(&p, 1, 0);
        if (ready <= 0) break;
    }
    return redraw;
}
