/*
 * xterm.c — render-only X11 terminal surface
 *
 *   - Pure libc, zero X11 libraries
 *   - Raw X11 wire protocol over Unix socket
 *   - BFNT bitmap font (produced by _font/mkfont.c)
 *   - File-backed BFNT font mapping; protocol buffers supplied by main's stack frame
 *   - One PutImage per tile of cells (no full-screen framebuffer)
 *
 * The font is loaded from /etc/bfnt/font2 (see BFNT font).  xterm_init() creates the
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
#include "xterm_conf.h"

/* Ugly workaround for xtmux that uses both dietlibc and _sys/ buildsystem */
#ifdef X11_BACKEND
extern char *getenv(const char *name);
#endif

#define AF_UNIX 1

/* Events always selected on our window (more are added for the pointer). */
#define EMASK_BASE ((1u<<17)|(1u<<15)|(1u<<0)|(1u<<4)) /* StructureNotify|Exposure|KeyPress|EnterWindow */

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

/* ── palette (0x00RRGGBB) ─────────────────────────────────────────── */

/* Standard xterm 256-colour palette (see e.g.
 * https://www.ditig.com/256-colors-cheat-sheet), matching the encoding
 * documented in vt.h for Cell.fg/bg: 0-7 ANSI, 8-15 bright ANSI,
 * 16-231 the 6x6x6 colour cube, 232-255 the greyscale ramp.  Nothing is
 * overridden: the defaults are slots XTERM_FG_DEFAULT and XTERM_BG_DEFAULT
 * (xterm.h), so SGR 39/49 and a plain cell render as slots 7 and 0. */
static uint32_t palette[256];

static void build_palette(void)
{
    static const uint32_t base16[16] = {
        0x1a1a1a, 0xcd0000, 0x00cd00, 0xcdcd00, 0x3465a4, 0xcd00cd, 0x6fabad, 0xcccccc,
        0x666666, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
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

/* read() and write() here are the raw syscalls, which return -errno.
 * EINTR is not a failure: it only means a signal handler ran while we
 * were blocked (say SIGWINCH, when the terminal that launched us is
 * resized -- a tiling WM does that whenever it rearranges windows), and
 * the kernel restarts the call only if the host program's handler was
 * installed with SA_RESTART, which not every libc's signal() does.  Any
 * program linking this must survive handlers of its own, so retry. */
#ifndef EINTR
#define EINTR 4
#endif
/* Once the connection is up (xup), losing it -- the server went away,
 * an `ssh -X` link dropped -- is not fatal to the program: reads and
 * writes become no-ops (a read returns zeros, which parse as an ignorable
 * "error" event) and xterm_lost() reports it, so the application can
 * decide what to do (xtmux detaches, leaving the session running).
 * During setup there is nothing to fall back on, so that still dies. */
static uint8_t xup, xlost;

static void xread(void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        long r = xlost ? 0 : read(xfd, p, n);
        if (r == -EINTR) continue;
        if (r <= 0) {
            if (!xup) die("xread failed\n");
            xlost = 1; memset(p, 0, n); return;
        }
        p += r; n -= r;
    }
}
static void xwrite(const void *buf, size_t n)
{
    const char *p = buf;
    while (n && !xlost) {
        long r = write(xfd, p, n);
        if (r == -EINTR) continue;
        if (r <= 0) {
            if (!xup) die("xwrite failed\n");
            xlost = 1; return;
        }
        p += r; n -= r;
    }
}

/* Every request we send gets the next sequence number (the server counts
 * them from 1 after the connection setup, which is not a request); the
 * replies and errors it sends carry that number.  That is how a reply is
 * told from an error that belongs to some earlier request -- see
 * await_reply. */
static uint16_t xseq;
static void xreq(const void *buf, size_t n) { xseq++; xwrite(buf, n); }
static int  await_reply(uint8_t r[32]);

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

/* ── PutImage (opcode 72, ZPixmap), auto-striped ───────────────────
 * The request header is built in the PUT_HDR bytes just in front of the
 * pixels (`px` must have them writable), so header and pixels leave in a
 * single write(). */
#define PUT_HDR 24
static void put_image(uint32_t draw, uint32_t gc,
                      int dstx, int dsty,
                      int w, int h, uint8_t depth, int bytes_per_pixel,
                      uint8_t *px)
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
        uint8_t *hdr = px + (size_t)y0 * stride - PUT_HDR;
        uint8_t  save[PUT_HDR];
        memcpy(save, hdr, PUT_HDR);     /* pixels, if a previous stripe */
        memset(hdr, 0, PUT_HDR);
        hdr[0] = 72; hdr[1] = 2;
        memcpy(hdr+ 2, &rlen, 2); memcpy(hdr+ 4, &draw, 4);
        memcpy(hdr+ 8, &gc,   4); memcpy(hdr+12, &uw,   2);
        memcpy(hdr+14, &ur,   2); memcpy(hdr+16, &sx,   2);
        memcpy(hdr+18, &sy,   2); hdr[21] = depth;
        xreq(hdr, PUT_HDR + dsz);
        memcpy(hdr, save, PUT_HDR);
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
    xreq(req, 8);

    uint8_t hdr[32];
    if (!await_reply(hdr)) return;   /* it failed: leave keysyms_per_kc == 0 */
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

/* The keypad keysyms: X's range 0xFF80-0xFFBD. */
static int is_keypad(uint32_t ks) { return ks >= 0xFF80 && ks <= 0xFFBD; }

#if XTERM_CAPSLOCK
/* A letter that CapsLock changes the case of: ASCII, and the Latin-1 letters
 * (0xE0-0xFE, without the division sign). */
static int is_letter(uint32_t ks)
{
    return (ks >= 'a' && ks <= 'z') || (ks >= 0xE0 && ks <= 0xFE && ks != 0xF7);
}
#endif

/* The keysym a key gives at the level its Shift and AltGr select.  A level the
 * keymap leaves empty (NoSymbol) takes the level below it, as Xlib does: the
 * arrows, Return and Escape have nothing in their shifted column, so Shift+Left
 * is still Left and Shift+Return still Return.  A keypad key types its digit,
 * point or operator only with NumLock on and Shift off, as xterm does; otherwise
 * it is the editing key of its base column (KP_7 is Home), and any Shift goes on
 * as a modifier of that key (see handle_keypress): Shift+KP_1 is ESC [ 1 ; 2 F.
 * With XTERM_CAPSLOCK, CapsLock reverses Shift for a letter.  0 if the key gives
 * no symbol. */
static uint32_t keysym_of(uint8_t keycode, int shift, int altgr, int numlock, int capslock)
{
    uint32_t base = keysym_for(keycode, 0);
    int lvl = (altgr ? 2 : 0) + (shift ? 1 : 0);
#if XTERM_NUMLOCK
    if (is_keypad(base)) lvl = (altgr ? 2 : 0) + (numlock && !shift ? 1 : 0);
#else
    (void)numlock;
    if (is_keypad(base)) lvl = 1;   /* as NumLock on, no modifier: see xterm_conf.h */
#endif
#if XTERM_CAPSLOCK
    if (capslock && is_letter(base)) lvl ^= 1;
#else
    (void)capslock;
#endif
    uint32_t ks = keysym_for(keycode, lvl);
    if (!ks && (lvl & 1)) ks = keysym_for(keycode, lvl & ~1);   /* shift level empty */
    if (!ks && (lvl & 2)) ks = keysym_for(keycode, 0);          /* AltGr level empty */
    return ks;
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

/* xterm's modifier parameter for a key: 1, plus 1 for Shift, 2 for Alt and 4
 * for Control.  Alt is passed with the Alt meta bit already resolved. */
static int mod_param(int shift, int alt, int ctrl)
{
    return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
}

/* Application cursor keys (DECCKM), as the program sets them: see
 * xterm_app_cursor() in xterm.h. */
static uint8_t app_cursor;
void xterm_app_cursor(int on) { app_cursor = on != 0; }

/* The cursor, editing and function keys, by the code of the sequence each
 * sends: a letter (the final byte of ESC [ x; F1-F4 end in P-S, and E is
 * KP_Begin's), or 1000 plus the number of ESC [ n ~ (Insert, Delete, PgUp,
 * PgDn, F5-F12).  0 for any other keysym. */
static int nav_code(uint32_t ks)
{
    switch (ks) {
    case 0xFF51: return 'D';            /* Left  */
    case 0xFF52: return 'A';            /* Up    */
    case 0xFF53: return 'C';            /* Right */
    case 0xFF54: return 'B';            /* Down  */
    case 0xFF50: return 'H';            /* Home  */
    case 0xFF57: return 'F';            /* End   */
    case 0xFF9D: return 'E';            /* KP_Begin: the 5 with NumLock off */
    case 0xFFBE: return 'P';            /* F1  */
    case 0xFFBF: return 'Q';            /* F2  */
    case 0xFFC0: return 'R';            /* F3  */
    case 0xFFC1: return 'S';            /* F4  */
    case 0xFF63: return 1000 + 2;       /* Insert  */
    case 0xFFFF: return 1000 + 3;       /* Delete  */
    case 0xFF55: return 1000 + 5;       /* Page_Up   */
    case 0xFF56: return 1000 + 6;       /* Page_Down */
    case 0xFFC2: return 1000 + 15;      /* F5  */
    case 0xFFC3: return 1000 + 17;      /* F6  */
    case 0xFFC4: return 1000 + 18;      /* F7  */
    case 0xFFC5: return 1000 + 19;      /* F8  */
    case 0xFFC6: return 1000 + 20;      /* F9  */
    case 0xFFC7: return 1000 + 21;      /* F10 */
    case 0xFFC8: return 1000 + 23;      /* F11 */
    case 0xFFC9: return 1000 + 24;      /* F12 */
    default: return 0;
    }
}

#if XTERM_NUMLOCK
/* The keypad's editing and function keys, as the keys they stand for: KP_Home
 * is Home, KP_F1 is F1.  (The keypad keysyms are in the order of the keys they
 * stand for, so this is arithmetic.)  Any other keysym is returned as it is. */
static uint32_t kp_nav(uint32_t ks)
{
    if (ks >= 0xFF95 && ks <= 0xFF9C) return 0xFF50 + (ks - 0xFF95);  /* KP_Home .. KP_End */
    if (ks >= 0xFF91 && ks <= 0xFF94) return 0xFFBE + (ks - 0xFF91);  /* KP_F1 .. KP_F4    */
    if (ks == 0xFF9E) return 0xFF63;                                  /* KP_Insert */
    if (ks == 0xFF9F) return 0xFFFF;                                  /* KP_Delete */
    return ks;
}
#define NAV_KEYSYM(ks) kp_nav(ks)
#else
#define NAV_KEYSYM(ks) (ks)
#endif

/* A cursor, editing or function key, sent as xterm does (code from nav_code).
 * Unmodified: the letter keys send ESC [ x, or ESC O x for F1-F4 and, with
 * DECCKM set, for the arrows, Home, End and KP_Begin; the others ESC [ n ~.  Modified:
 * ESC [ 1 ; m x or ESC [ n ; m ~, with m from mod_param (Shift+Up is
 * ESC [ 1 ; 2 A, Ctrl+F5 is ESC [ 15 ; 5 ~). */
static void push_nav(int code, int mod)
{
    if (code >= 1000) {
        int n = code - 1000;
        key_push(0x1B); key_push('[');
        if (n >= 10) key_push((uint8_t)('0' + n / 10));
        key_push((uint8_t)('0' + n % 10));
        if (mod != 1) { key_push(';'); key_push((uint8_t)('0' + mod)); }
        key_push('~');
        return;
    }
    int ss3 = (code >= 'P' && code <= 'S') ||
              (app_cursor && (code == 'A' || code == 'B' || code == 'C' ||
                              code == 'D' || code == 'E' || code == 'F' || code == 'H'));
    key_push(0x1B);
    if (mod == 1) {
        key_push(ss3 ? 'O' : '[');
    } else {
        key_push('['); key_push('1'); key_push(';'); key_push((uint8_t)('0' + mod));
    }
    key_push((uint8_t)code);
}

/* The keypad keys that type something, as the ordinary key they stand for.
 * (KP_0 .. KP_9 are the digits, which NumLock selects: see keysym_of.) */
static uint32_t keypad_char(uint32_t ks)
{
    if (ks >= 0xFFB0 && ks <= 0xFFB9) return '0' + (ks - 0xFFB0);
    if (ks >= 0xFFAA && ks <= 0xFFAF) return "*+,-./"[ks - 0xFFAA];
    switch (ks) {
    case 0xFF80: return ' ';      /* KP_Space */
    case 0xFF89: return 0xFF09;   /* KP_Tab: Tab */
    case 0xFF8D: return 0xFF0D;   /* KP_Enter: Return */
    case 0xFFBD: return '=';      /* KP_Equal */
    default: return ks;
    }
}

/* The bytes a typed key sends: printable Latin-1 (ASCII as-is, 0xA0-0xFF
 * UTF-8-encoded), Ctrl-letter control codes and the few control keys.  Built
 * in a buffer, so that handle_keypress() can decide about an Alt ESC prefix
 * knowing whether the key sends anything at all (a bare modifier does not). */
typedef struct { uint8_t b[8]; int n; } KeyOut;

static void key_bytes(KeyOut *o, uint32_t ks, int ctrl)
{
    if (ctrl) {
        /* xterm's control codes: a letter, or one of @ [ \ ] ^ _ ` { | } ~,
         * is its value & 0x1f; the digits 2 to 8 and ? and / have codes of
         * their own, and Ctrl+Space is NUL.  Other keys send their character. */
        uint32_t up = ks;
        if (up >= 'a' && up <= 'z') up -= 'a' - 'A';
        if (up >= '@' && up <= '~') { o->b[o->n++] = (uint8_t)(up & 0x1f); return; }
        if (up >= '2' && up <= '8') {
            static const char digit_ctrl[] = "\0\x1b\x1c\x1d\x1e\x1f\x7f";
            o->b[o->n++] = (uint8_t)digit_ctrl[up - '2']; return;
        }
        if (up == '/') { o->b[o->n++] = 0x1f; return; }
        if (up == '?') { o->b[o->n++] = 0x7f; return; }
        if (up == ' ') { o->b[o->n++] = 0; return; }
    }

    if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xA0 && ks <= 0xFF)) {
        if (ks <= 0x7F) {
            o->b[o->n++] = (uint8_t)ks;
        } else {
            o->b[o->n++] = (uint8_t)(0xC0 | (ks >> 6));
            o->b[o->n++] = (uint8_t)(0x80 | (ks & 0x3F));
        }
        return;
    }

    switch (ks) {
    case 0xFF08: o->b[o->n++] = ctrl ? 0x08 : 0x7f; return;   /* BackSpace; ^H with Ctrl, as xterm */
    case 0xFF09: o->b[o->n++] = '\t'; return;   /* Tab */
    case 0xFE20:                                /* Shift+Tab: back-tab, ESC [ Z */
        o->b[o->n++] = 0x1b; o->b[o->n++] = '['; o->b[o->n++] = 'Z'; return;
    case 0xFF0D: o->b[o->n++] = '\r'; return;   /* Return */
    case 0xFF1B: o->b[o->n++] = 0x1b; return;   /* Escape */
    default: return;
    }
}

/* Translate one KeyPress event (32 bytes) into 0+ output bytes.
 *
 * The keysym comes from keysym_of(), which picks the level the modifiers
 * select.  Then, in order:
 *   - the paste keys, which are not typed to the program at all;
 *   - the Ctrl+font-size keys, which are the multiplexer's own;
 *   - cursor, editing and function keys (nav_code, kp_nav): their sequences,
 *     with xterm's modifier parameter when any modifier is held (push_nav);
 *   - typed keys (key_bytes): printable Latin-1 (ASCII as-is, 0xA0-0xFF
 *     UTF-8-encoded, as everything downstream expects on the wire), Ctrl-
 *     letter control codes, and the few control keys.  An Alt ESC prefix
 *     goes only before a key that sends something.
 * Anything else is silently ignored.
 *
 * X11's Latin-1 keysyms (0x020-0x0FF) are numerically identical to their
 * Unicode code points, so ks doubles as the code point with no translation
 * table needed. */
/* Paste shortcuts: Shift+Insert, and Ctrl+V with no other modifier (not
 * Shift, Alt, Super...: those reach the program, as plain Ctrl+V if
 * nothing else is bound to them).  They are not typed to the program; the
 * application is told (xterm_paste_key) and pastes as it does for the
 * mouse.  Compile with -DXTERM_PASTE_CTRL_V=0 to leave Ctrl+V to the
 * program (vim's literal-next, for one). */
#ifndef XTERM_PASTE_CTRL_V
#define XTERM_PASTE_CTRL_V 1
#endif
static uint8_t paste_key;
/* Font size steps asked for by the font keys (see handle_keypress), not yet
 * applied: xterm_wait() changes the size. */
static int     font_delta;

int xterm_paste_key(void)
{
    int k = paste_key;
    paste_key = 0;
    return k;
}

static void handle_keypress(const uint8_t *ev)
{
    uint8_t keycode = ev[1];
    uint16_t state; memcpy(&state, ev+28, 2);
    int shift = (state & 0x0001) != 0;
    int ctrl  = (state & 0x0004) != 0;
    int alt   = (state & 0x0008) != 0;  /* Mod1: conventional "Alt" binding */
    int capslock = XTERM_CAPSLOCK && (state & 0x0002) != 0;  /* Lock: see keysym_of */
    int numlock = (state & 0x0010) != 0;  /* Mod2: NumLock, see keysym_of */
    int altgr = (state & 0x0080) != 0;  /* Mod5: conventional AltGr /
                                          * ISO_Level3_Shift binding, as
                                          * produced by e.g. xkb option
                                          * "lv3:ralt_switch" */

    uint32_t ks = keysym_of(keycode, shift, altgr, numlock, capslock);
    /* Insert usually has nothing in its shifted column, so Shift+Insert has
     * to be recognized by the unshifted one. */
    int insert = ks == 0xFF63 ||
                 (shift && keysym_for(keycode, altgr ? 2 : 0) == 0xFF63);
    if ((shift && !ctrl && !alt && insert) ||                              /* Shift+Insert */
        (XTERM_PASTE_CTRL_V && ctrl && !(state & 0xE9) &&   /* Ctrl+V, and of the other modifier bits */
         (ks == 'v' || ks == 'V'))) {                       /* only CapsLock/NumLock (0x02, 0x10) */
        paste_key = 1;
        return;
    }

#if XTERM_FONT_KEYS
    /* Font size: Ctrl+= and Ctrl+KP_Add one size larger, Ctrl+- and Ctrl+KP_Subtract
     * one smaller.  Unshifted only, so that Ctrl+_ (Ctrl+Shift+-, the readline and
     * Emacs undo key) reaches the program, as the other shifted forms do.  Like the
     * paste keys these are not typed to the program; the size is changed in
     * xterm_wait(), where the grid is re-derived. */
    if (ctrl && !shift && !alt) {
        if (ks == '=' || ks == 0xFFAB) { font_delta++; return; }   /* KP_Add */
        if (ks == '-' || ks == 0xFFAD) { font_delta--; return; }   /* KP_Subtract */
    }
#endif

    if (!ks) return;

    /* The cursor, editing and function keys carry their modifiers in the
     * sequence, Alt included: ESC is not prefixed to them. */
    int nav = nav_code(NAV_KEYSYM(ks));
    if (nav) {
        push_nav(nav, mod_param(shift, alt && !altgr, ctrl));
        return;
    }

    /* A keypad key that types something sends just its character, whatever
     * the modifiers (xterm: Ctrl+KP_Divide is "/", not ^_). */
    if (is_keypad(keysym_for(keycode, 0))) { ctrl = 0; alt = 0; }

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
    KeyOut o = { {0}, 0 };
    key_bytes(&o, keypad_char(ks), ctrl);
    if (alt && !altgr && o.n) key_push(0x1B);   /* only a key that sends something */
    for (int i = 0; i < o.n; i++) key_push(o.b[i]);
}

/* ═══════════════════════════════════════════════════════════════════
 * BFNT font
 * ═══════════════════════════════════════════════════════════════════ */

#define GRID_MAX_CELLS (32 * 1024)
#define SETUP_MAX      (8 * 1024)
/* The bytes of one tile of cells, see cell_storage and render_all(). */
#define TILE_BYTES     (PUT_HDR + 64*64*4)

/* The font comes in FONT_SIZES sizes (see _font/mkfont.c), each in files of
 * its own: /etc/bfnt/font<k> and /etc/bfnt/emoji<k> for size k.  Size k's
 * cell is 2^((k - BASE_SIZE)/4) times the base size's in both directions.
 * The font starts at BASE_SIZE.  The cell size is in the file's header, so
 * the name only says which size the file is. */
#define FONT_SIZES     8
#define BASE_SIZE      2
#define BFNT_DIR       "/etc/bfnt/"

static uint8_t  *font_data;     /* the font in use, mapped; NULL until one is loaded */
static size_t    font_len;
static uint16_t  cell_w, cell_h, baseline, first_cp, num_glyphs, num_wide;
static uint8_t  *glyph_data;
static uint8_t  *wide_data;     /* num_wide glyphs of 2*cell_w x cell_h, after the narrow ones */
static int       WIN_W, WIN_H, cols, rows;
static int       cfg_w, cfg_h;  /* window size as last reported by the server */
static int       font_size = BASE_SIZE;  /* the size in use */
static char      font_path[]  = BFNT_DIR "font2";     /* the digit is the size: */
static char      emoji_path[] = BFNT_DIR "emoji2";   /* see font_switch() */
#define FONT_DIGIT  (sizeof BFNT_DIR "font" - 1)
#define EMOJI_DIGIT (sizeof BFNT_DIR "emoji" - 1)

#ifndef SEEK_END
#define SEEK_END 2
#endif

/* A 16-bit little-endian field of a BFNT or EMOJ header. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Map the whole of a file read-only.  Its length comes from lseek, so the
 * mapping is exactly the file: nothing is read past its end, which would be
 * SIGBUS, whatever a header claims.  Returns NULL, with *len set, on failure. */
static uint8_t *map_file(const char *path, size_t *len)
{
    int fd = (int)open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long n = lseek(fd, 0, SEEK_END);
    uint8_t *m = n > 0 ? mmap(0, (size_t)n, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return NULL;
    *len = (size_t)n;
    return m;
}

/* Map and check the font at path, without changing the font in use: the
 * caller commits it with font_commit(), or unmaps it.  The check covers all
 * that the renderer relies on, so a font that passes can be drawn. */
static uint8_t *font_open(const char *path, size_t *len)
{
    uint8_t *m = map_file(path, len);
    if (!m) return NULL;
    size_t n = *len;
    uint16_t w = n >= 16 ? rd16(m+4) : 0, h = n >= 16 ? rd16(m+6) : 0;
    if (n < 16 || m[0] != 'B' || m[1] != 'F' || m[2] != 'N' || m[3] != 'T' ||
        w == 0 || h == 0 ||
        /* the glyphs are all in the file: (n-16) / (w*h) bitmaps fit in it */
        (n - 16) / h / w < (size_t)rd16(m+12) + 2 * (size_t)rd16(m+14) ||
        /* a cell fits a tile at the widest depth, with the tile's padding (tile_fit) */
        (TILE_BYTES - PUT_HDR) / h < 3 + (size_t)w * 4) {
        munmap(m, n);
        return NULL;
    }
    return m;
}

/* Make m, from font_open(), the font in use; the one it replaces is unmapped. */
static void font_commit(uint8_t *m, size_t len)
{
    if (font_data) munmap(font_data, font_len);
    font_data = m; font_len = len;
    cell_w = rd16(m+4); cell_h = rd16(m+6); baseline = rd16(m+8); first_cp = rd16(m+10);
    num_glyphs = rd16(m+12); num_wide = rd16(m+14);     /* 0 in a file without wide glyphs */
    glyph_data = m + 16;
    wide_data  = glyph_data + (size_t)cell_w * cell_h * num_glyphs;
}

/* emoji<k>.bfnt (see _font/mkemoji.c): optional.  If it is missing, or does
 * not match the font's cell size, every emoji draws as a placeholder box (see
 * draw_box) rather than killing the terminal.  It replaces the emoji in use
 * whatever happens, so with no good file there are no emoji at all. */
#define EMOJI_HDR 12
static const uint8_t *emoji_data;   /* NULL = no emoji available; the mapping's start */
static size_t   emoji_len;
static uint16_t emoji_count;
static size_t   emoji_rec;          /* bytes per record: 45 palette + packed 4bpp pixels */

static void emoji_load(const char *path)
{
    if (emoji_data) { munmap((void *)emoji_data, emoji_len); emoji_data = NULL; }
    size_t len;
    uint8_t *m = map_file(path, &len);
    if (!m) return;
    if (len < EMOJI_HDR || m[0] != 'E' || m[1] != 'M' || m[2] != 'O' || m[3] != 'J' ||
        rd16(m+4) != cell_w || rd16(m+6) != cell_h) {
        munmap(m, len);
        return;
    }
    size_t rec = 45 + ((size_t)cell_w * 2 * cell_h + 1) / 2;
    size_t count = rd16(m+8);
    if (count * rec > len - EMOJI_HDR) {          /* the records are all in the file */
        munmap(m, len);
        return;
    }
    emoji_data = m; emoji_len = len; emoji_count = (uint16_t)count; emoji_rec = rec;
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
    memset(fgb, XTERM_FG_DEFAULT, cells);
    memset(bgb, XTERM_BG_DEFAULT, cells);
    memset(atb, 0,  cells);
}

/* ═══════════════════════════════════════════════════════════════════
 * Per-cell rendering
 * ═══════════════════════════════════════════════════════════════════ */

/* Cells of one text row are drawn side by side into a tile that is sent
 * with a single PutImage; cell_buf is the current cell's top-left corner
 * inside it and cell_stride the tile's scanline pitch.  The tile is also
 * the only pixel storage: there is no full-screen framebuffer. */
static uint8_t cell_storage[TILE_BYTES] __attribute__((aligned(4)));
#define TILE (cell_storage + PUT_HDR)
static uint8_t *cell_buf;
static size_t   cell_stride;
static int      batch;          /* cells per tile, set by xterm_init */

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

/* Glyph coverage (0-255) -> final pixel, for the fg/bg pair of the cell
 * being drawn.  Entries are computed on first use and the table is
 * invalidated only when that pair changes (runs of same-coloured text,
 * i.e. nearly everything, reuse it), so blending costs one table load per
 * pixel and the background fill shares the glyph's single pass: entry 0
 * is the plain background.  A glyph only uses a few dozen of the 256
 * coverage values, which is why filling lazily beats building it whole
 * when the colours change from cell to cell.  ~0 marks an empty entry (no
 * pixel value, 0x00RRGGBB or RGB565, can be that). */
static uint32_t lut[256];
static uint32_t lut_key = ~0u;
static uint32_t lut_fg, lut_bg;

static void lut_set(uint8_t fgi, uint8_t bgi)
{
    uint32_t key = (uint32_t)fgi << 8 | bgi;
    if (key == lut_key) return;
    lut_key = key;
    lut_fg = palette[fgi]; lut_bg = palette[bgi];
    for (int i = 0; i < 256; i++) lut[i] = ~0u;
}

static uint32_t lut_fill(uint8_t a, uint8_t depth)
{
    uint32_t p = blend(a, lut_fg, lut_bg);
    return lut[a] = depth == 16 ? pack565(p) : p;
}

/* Store one pixel (x,y in cell-local coordinates) at the window depth. */
static inline void put_px(int x, int y, uint32_t color, uint8_t depth)
{
    uint8_t *line = cell_buf + (size_t)y * cell_stride;
    if (depth == 16) ((uint16_t *)line)[x] = pack565(color);
    else             ((uint32_t *)line)[x] = color;
}

/* Solid horizontal line, full cell width, at cell-local row `r` (0 = top
 * of cell). Used for ATTR_UNDERLINE / ATTR_STRIKE — cheap structural
 * marks that a colour change alone can't express. */
static void fill_row(int r, uint32_t color, uint8_t depth)
{
    if (r < 0 || r >= cell_h) return;
    for (int x = 0; x < cell_w; x++) put_px(x, r, color, depth);
}

/* Paint the whole cell from a glyph's coverage bitmap through lut[].
 * Cell column x shows glyph column x - sh (blank outside the glyph).
 * `stride` is the bitmap's row length: cell_w for a narrow glyph, or
 * 2*cell_w for a wide one (g then points at the column the cell shows).
 * Written out per pixel size (GLYPH_ROW) so the depth test is per row,
 * not per pixel. */
#define GLYPH_ROW(T)                                                   \
    for (int x = 0; x < cell_w; x++) {                                 \
        unsigned gx = (unsigned)(x - sh);                              \
        uint8_t a = gx < cell_w ? g[gx] : 0;                           \
        uint32_t p = lut[a];                                           \
        if (p == ~0u) p = lut_fill(a, depth);                          \
        ((T *)line)[x] = (T)p;                                         \
    }
__attribute__((optimize("O2"))) /* the hot loop: -Os costs ~30% here */
static void draw_glyph(const uint8_t *g, int stride, int sh, uint8_t depth)
{
    for (int y = 0; y < cell_h; y++, g += stride) {
        uint8_t *line = cell_buf + (size_t)y * cell_stride;
        if (depth == 16) { GLYPH_ROW(uint16_t) }
        else             { GLYPH_ROW(uint32_t) }
    }
}
#undef GLYPH_ROW

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
 * stored, so it needs no data (and works with no emoji file at all)
 * and follows fg colour, including the cursor's inversion. */
static void draw_box(int off, int w, uint32_t fg, uint8_t depth)
{
    int last = w - 2;                          /* rightmost line column */
    for (int y = 1; y < cell_h - 1; y++)
        for (int x = 0; x < cell_w; x++) {
            int X = off + x;
            if (X >= 1 && X <= last && (X == 1 || X == last || y == 1 || y == cell_h - 2))
                put_px(x, y, fg, depth);
        }
}

static void draw_emoji(int k, int half, uint32_t fg, uint8_t depth)
{
    if (!emoji_data || k >= emoji_count) { draw_box(half * cell_w, cell_w * 2, fg, depth); return; }
    const uint8_t *rec = emoji_data + EMOJI_HDR + (size_t)k * emoji_rec;
    const uint8_t *pix = rec + 45;
    int gw = cell_w * 2;
    for (int y = 0; y < cell_h; y++)
        for (int x = 0; x < cell_w; x++) {
            int p  = y * gw + half * cell_w + x;
            int ix = (p & 1) ? pix[p >> 1] & 15 : pix[p >> 1] >> 4;
            if (!ix) continue;
            const uint8_t *c = rec + (ix - 1) * 3;
            put_px(x, y,
                   (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2], depth);
        }
}

/* Cursor: a plain full-block inversion (fg/bg swapped for the whole
 * cell), no blinking. row<0 means "no cursor" (hidden/unfocused etc). */
static int cursor_row = -1, cursor_col = -1;

static void render_cell(int row, int col, uint8_t depth)
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
    uint8_t fgi = fgb[idx], bgi = bgb[idx];
    if (is_cursor) { uint8_t t = bgi; bgi = fgi; fgi = t; }
    lut_set(fgi, bgi);
    uint32_t fg = palette[fgi];
    uint8_t cp = scr[idx];
    /* Anything that isn't an ordinary glyph (emoji, placeholder box) is
     * drawn over a plain background, which is what glyph slot 0 gives:
     * it is the space, see charset.h, so its coverage is all zero. */
    const uint8_t *g = glyph_data;
    int sh = 0, stride = cell_w;
    if (!is_emoji && !(rattrs & XTERM_ATTR_MISSING) && (rattrs & XTERM_ATTR_WIDE)) {
        /* A wide glyph: this cell's half of a two-cell bitmap. An index
         * past the font's wide glyphs leaves g on slot 0, a blank. */
        if (cp < num_wide) {
            g = wide_data + (size_t)cp * 2 * cell_w * cell_h + (size_t)half * cell_w;
            stride = 2 * cell_w;
        }
    } else if (!is_emoji && !(rattrs & XTERM_ATTR_MISSING) &&
        cp >= first_cp && cp < (uint16_t)(first_cp + num_glyphs)) {
        g += (size_t)(cp-first_cp)*cell_w*cell_h;
        /* A fullwidth character reuses its narrow twin's glyph, centred
         * across the two cells; this cell shows glyph columns x - sh
         * (left half: shifted right by half a cell; right half: the
         * remainder). Ordinary cells: sh = 0, every column in range. */
        if (is_fw) sh = cell_w / 2 - half * cell_w;
    }
    draw_glyph(g, stride, sh, depth);
    if (is_emoji)
        draw_emoji(cp, half, fg, depth);
    else if (rattrs & XTERM_ATTR_MISSING)
        draw_box(0, cell_w, fg, depth);
    if (rattrs & XTERM_ATTR_UNDERLINE) {
        int r = baseline + 1;
        fill_row(r >= cell_h ? cell_h - 1 : r, fg, depth);
    }
    if (rattrs & XTERM_ATTR_STRIKE) {
        fill_row(baseline / 2, fg, depth);
    }
}

/* Rows whose content is unchanged since they were last sent are skipped.
 * "Content" is what render_cell reads for the row (glyph, colours and
 * attributes of every cell, plus the cursor column if the cursor is on
 * it), folded into a 32-bit FNV-1a hash per row: 4 bytes of state per row
 * rather than a shadow copy of the grid.  A collision would leave one row
 * stale until it next changes -- harmless, and ~2^-32 per update.
 * Rows past ROWS_MAX are simply always drawn.  `synced` is cleared by
 * Expose (the server dropped our pixels), which forces a full repaint. */
#define ROWS_MAX 256
static uint32_t row_hash[ROWS_MAX];
static uint8_t  synced;

static uint32_t hash_row(int r)
{
    uint8_t *const plane[4] = { scr, fgb, bgb, atb };
    uint32_t h = 2166136261u;
    if (r == cursor_row) h = (h ^ (uint32_t)(cursor_col + 1)) * 16777619u;
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < cols; c++)
            h = (h ^ plane[i][r * cols + c]) * 16777619u;
    return h;
}

static void render_all(uint32_t draw, uint32_t gc, uint8_t depth)
{
    int bpp = depth == 16 ? 2 : 4;
    for (int r = 0; r < rows; r++) {
        if (r < ROWS_MAX) {
            uint32_t h = hash_row(r);
            if (synced && row_hash[r] == h) continue;
            row_hash[r] = h;
        }
        for (int c0 = 0; c0 < cols; c0 += batch) {
            int nc = cols - c0 < batch ? cols - c0 : batch;
            cell_stride = ((size_t)nc * cell_w * bpp + 3) & ~(size_t)3;
            for (int c = 0; c < nc; c++) {
                cell_buf = TILE + (size_t)c * cell_w * bpp;
                render_cell(r, c0 + c, depth);
            }
            put_image(draw, gc, c0*cell_w, r*cell_h, nc*cell_w, cell_h,
                      depth, bpp, TILE);
        }
    }
    synced = 1;
}


/* Library-owned state and backing storage. */
static uint8_t screen_storage[GRID_MAX_CELLS];
static uint32_t xterm_win, xterm_gc;

/* Re-derive the grid from a window size in pixels.  Unlike the initial
 * grid_init() this never dies: a window can be made arbitrarily small or
 * large, so the result is clamped to between one cell and what the
 * buffers hold.  Returns nonzero if the grid dimensions changed (the
 * cell buffers are then reset: their row stride changed with `cols`, and
 * the caller is about to rewrite every cell anyway). */
static int regrid(int w, int h)
{
    if (w < cell_w) w = cell_w;
    if (h < cell_h) h = cell_h;
    int c = w / cell_w, r = h / cell_h;
    if ((size_t)c * r > sizeof screen_storage) r = (int)(sizeof screen_storage) / c;
    if (c == cols && r == rows) return 0;
    grid_init((uint16_t)(c * cell_w), (uint16_t)(r * cell_h),
              screen_storage, sizeof screen_storage);
    synced = 0;
    return 1;
}

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
 * than misreading its bytes as if they were our reply.  They are not
 * thrown away, though: a window manager that resizes the window right
 * after mapping it can have its ConfigureNotify arrive here, and
 * handle_event() records it for xterm_init to apply. */
static void handle_event(const uint8_t *ev);
/* The reply to the request just sent.  What else arrives first is dealt
 * with: events are handled, and an error for an *earlier* request (one of
 * the many we fire off without waiting, such as SetInputFocus on a window
 * the window manager has not mapped yet) is dropped -- it must not be
 * taken for the answer.  Returns 0 if the awaited request itself failed,
 * or the connection is gone. */
static int await_reply(uint8_t r[32])
{
    for (;;) {
        xread(r, 32);
        if (xlost) return 0;
        if (r[0] == 1) return 1;
        if (r[0] == 0) {
            uint16_t seq;
            memcpy(&seq, r + 2, 2);
            if (seq == xseq) return 0;
            continue;
        }
        handle_event(r);
    }
}

static uint32_t intern_atom(const char *name, int len)
{
    uint8_t req[24] = {0};
    uint16_t reqlen = (uint16_t)(2 + ((len + 3) / 4));
    uint16_t nlen = (uint16_t)len;
    req[0] = 16; req[1] = 0; /* only-if-exists = 0 (create it) */
    memcpy(req+2, &reqlen, 2);
    memcpy(req+4, &nlen, 2);
    memcpy(req+8, name, (size_t)len);
    xreq(req, (size_t)reqlen * 4);
    uint8_t rep[32];
    if (!await_reply(rep)) return 0;
    uint32_t atom;
    memcpy(&atom, rep+8, 4);
    return atom;
}

static uint32_t wm_protocols_atom, wm_delete_window_atom;
static uint32_t a_clipboard, a_utf8, a_targets, a_prop;   /* see "Selections" */
static int close_requested;

/* WM_NAME (STRING, format 8) — bare-minimum window title. */
void xterm_set_title(const char *name, int len)
{
    uint8_t buf[24 + 256] = {0};     /* the padding after the name goes to the server: no stale bytes */
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
    xreq(buf, (size_t)reqlen * 4);
}

int xterm_close_requested(void) { return close_requested; }
static uint8_t xterm_depth;

/* The size to start at: the one whose grid is nearest XTERM_START_LINES lines
 * on a screen screen_h pixels high (the screen's height is the only input: the
 * DPI the server reports is often wrong).  Size k's cells are the base size's
 * times size_mul[k] / 1000, the same as mkfont's 2^((k-BASE_SIZE)/4) rounded to
 * thousandths; the files round to whole pixels, which is as close as this needs.
 * All the arithmetic is 32-bit, which every target has (64-bit division would
 * need a libgcc routine on some of them).  Ties go to the smaller size. */
static const uint16_t size_mul[FONT_SIZES] = { 707, 841, 1000, 1189, 1414, 1682, 2000, 2378 };

static int size_for_screen(uint16_t screen_h)
{
    int best = BASE_SIZE;
    unsigned best_err = ~0u;
    for (int k = 0; k < FONT_SIZES; k++) {
        unsigned h     = ((unsigned)cell_h * size_mul[k] + 500) / 1000;
        unsigned lines = (unsigned)screen_h * 1000 / h;           /* thousandths of a line */
        unsigned want  = XTERM_START_LINES * 1000u;
        unsigned err   = lines > want ? lines - want : want - lines;
        if (err < best_err) { best_err = err; best = k; }
    }
    return best;
}

/* Whole cells per tile: each scanline may need up to 3 bytes of padding.
 * Cells are drawn a tile at a time (see xterm_render), so this follows the font. */
static void tile_fit(void)
{
    batch = (int)(((TILE_BYTES - PUT_HDR) / cell_h - 3) /
                  ((size_t)cell_w * (xterm_depth == 16 ? 2 : 4)));
}

/* ClearArea (opcode 61), exposures off, over the whole window (width and height
 * 0 extend it to the right and bottom edges).  The window goes back to its
 * background, so nothing drawn at the old size stays visible past the new grid. */
static void clear_window(void)
{
    uint8_t r[16] = {0}; uint16_t len = 4;
    r[0] = 61;
    memcpy(r+2, &len, 2); memcpy(r+4, &xterm_win, 4);
    xreq(r, 16);
}

/* Whether a grid of cw x ch cells fits the grid buffer in the window (cfg_w x
 * cfg_h).  regrid() would truncate a grid that does not, leaving the bottom of
 * the window blank, so a size that needs it is refused instead. */
static int grid_fits(int cw, int ch, int win_w, int win_h)
{
    int c = win_w / cw, r = win_h / ch;
    if (c < 1) c = 1;
    if (r < 1) r = 1;
    return (size_t)c * r <= GRID_MAX_CELLS;
}

/* Make size k the one in use, if its font loads and its grid fits the grid
 * buffer in a win_w x win_h window.  Otherwise the size stays as it was, and 0 is
 * returned.  The emoji follow the font (no good file: they draw as boxes).  The
 * caller re-derives the grid from the window. */
static int font_switch(int k, int win_w, int win_h)
{
    if (k == font_size) return 0;
    font_path[FONT_DIGIT] = emoji_path[EMOJI_DIGIT] = (char)('0' + k);
    size_t len;
    uint8_t *m = font_open(font_path, &len);
    if (m && !grid_fits(rd16(m+4), rd16(m+6), win_w, win_h)) { munmap(m, len); m = NULL; }
    if (!m) {
        font_path[FONT_DIGIT] = emoji_path[EMOJI_DIGIT] = (char)('0' + font_size);
        return 0;
    }
    font_commit(m, len);
    emoji_load(emoji_path);
    font_size = k;
    tile_fit();
    return 1;
}

/* A font size step from a key (see xterm_wait): clamped to the sizes there are.
 * The grid follows in xterm_wait(), from the window's size; nothing else is done
 * if the size does not change.  Returns nonzero if it changed. */
static int font_size_set(int k)
{
    if (k < 0) k = 0;
    if (k >= FONT_SIZES) k = FONT_SIZES - 1;
    if (!font_switch(k, cfg_w, cfg_h)) return 0;
    WIN_W = cols * cell_w;
    WIN_H = rows * cell_h;
    synced = 0;                         /* every row is drawn again */
    clear_window();
    return 1;
}

uint8_t *xterm_framebuffer(void) { return scr; }
/* Parallel to xterm_framebuffer(): one xterm 256-colour palette index
 * per cell. Defaults (XTERM_FG_DEFAULT/XTERM_BG_DEFAULT, xterm.h) fill
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
    xreq(foc,12);
}

int xterm_init(void)
{
    uint8_t setup_storage[SETUP_MAX];
    x_max_request_words = 65535;
    build_palette();
    size_t flen;
    uint8_t *fm = font_open(font_path, &flen);
    if (!fm) die("could not load a BFNT font: /etc/bfnt/font2\n");
    font_commit(fm, flen);
    emoji_load(emoji_path);

    /* Close-on-exec (SOCK_CLOEXEC has O_CLOEXEC's value): the shells and
     * programs we start must not inherit the connection.  They would keep
     * it, and so the window, alive after we have detached or exited. */
    xfd = socket(AF_UNIX, SOCK_STREAM | O_CLOEXEC, 0);
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
    if (setup_size < 32) die("X11 setup reply is malformed\n");
    memcpy(&xid_base, d+4, 4); memcpy(&xid_mask, d+8, 4);
    uint16_t vlen; memcpy(&vlen, d+16, 2);
    uint16_t max_request_words; memcpy(&max_request_words, d+18, 2);
    if (max_request_words > 6) x_max_request_words = max_request_words;
    uint8_t nfmt = d[21];
    size_t soff = 32 + ((vlen+3u)&~3u) + (size_t)nfmt*8;
    /* The first screen's fields (up to the root depth at +38) must lie inside
     * the reply: a vendor string or format list longer than the reply says is
     * refused, not read past. */
    if (soff + 40 > setup_size) die("X11 setup reply is malformed\n");
    uint32_t root_win; memcpy(&root_win, d+soff, 4);
    uint32_t black_px; memcpy(&black_px, d+soff+12, 4);
    uint16_t screen_w; memcpy(&screen_w, d+soff+20, 2);
    uint16_t screen_h; memcpy(&screen_h, d+soff+22, 2);
    xterm_depth = d[soff+38];
    /* Start at the size that suits this screen.  If that size's font will not
     * load, the base size stays, which is loaded already. */
    font_switch(size_for_screen(screen_h), screen_w, screen_h);
    tile_fit();
    if (batch < 1) die("font cell is too large\n");
    min_keycode = d[26];
    max_keycode = d[27];
    load_keyboard_mapping();
    grid_init(screen_w, screen_h, screen_storage, sizeof screen_storage);

    xterm_win = new_xid(); xterm_gc = new_xid();
    uint8_t r[40] = {0};
    uint16_t len=10;
    int16_t x=(int16_t)((screen_w-WIN_W)/2), y=(int16_t)((screen_h-WIN_H)/2);
    uint16_t w=(uint16_t)WIN_W, h=(uint16_t)WIN_H, bw=0, cls=1;
    cfg_w = WIN_W; cfg_h = WIN_H;
    uint32_t vis=0, vmask=(1u<<1)|(1u<<11), bg=black_px;
    uint32_t emask=EMASK_BASE;
    r[0]=1;
    memcpy(r+2,&len,2); memcpy(r+4,&xterm_win,4); memcpy(r+8,&root_win,4);
    memcpy(r+12,&x,2); memcpy(r+14,&y,2); memcpy(r+16,&w,2); memcpy(r+18,&h,2);
    memcpy(r+20,&bw,2); memcpy(r+22,&cls,2); memcpy(r+24,&vis,4);
    memcpy(r+28,&vmask,4); memcpy(r+32,&bg,4); memcpy(r+36,&emask,4);
    xreq(r, 40);
    uint8_t map[8] = {0}; uint16_t map_len=2; map[0]=8;
    memcpy(map+2,&map_len,2); memcpy(map+4,&xterm_win,4); xreq(map,8);
    uint8_t gc[16] = {0}; uint16_t gc_len=4; uint32_t mask=0; gc[0]=55;
    memcpy(gc+2,&gc_len,2); memcpy(gc+4,&xterm_gc,4); memcpy(gc+8,&xterm_win,4);
    memcpy(gc+12,&mask,4); xreq(gc,16);

#ifdef XTERM_TEST_ERROR_BEFORE_ATOMS
    /* Test hook: an unrelated request that fails, as the SetInputFocus below
     * does on a window manager that has not mapped the window yet.  The
     * atoms interned after it must still come out right (see await_reply). */
    { uint8_t g[8] = {0}; uint16_t gl = 2; uint32_t bad = 0x7ffffff0;
      g[0] = 14; memcpy(g+2, &gl, 2); memcpy(g+4, &bad, 4); xreq(g, 8); }
#endif
    grab_focus();

    /* Opt into the WM_DELETE_WINDOW handshake so a window-manager-driven
     * close (clicking the window's close button, Alt-F4, etc) arrives as
     * an ordinary ClientMessage event instead of the connection just
     * dying under us. */
    wm_protocols_atom     = intern_atom("WM_PROTOCOLS", 12);
    wm_delete_window_atom = intern_atom("WM_DELETE_WINDOW", 16);
    a_clipboard = intern_atom("CLIPBOARD", 9);
    a_utf8      = intern_atom("UTF8_STRING", 11);
    a_targets   = intern_atom("TARGETS", 7);
    a_prop      = intern_atom("XTERM_PASTE", 11);
    uint8_t wp[28] = {0}; uint16_t wp_len=7; uint32_t wp_n=1;
    wp[0]=18; /* ChangeProperty, mode=Replace */
    memcpy(wp+2,&wp_len,2); memcpy(wp+4,&xterm_win,4);
    memcpy(wp+8,&wm_protocols_atom,4);
    uint32_t atom_type=4; memcpy(wp+12,&atom_type,4); /* type=ATOM */
    wp[16]=32; /* format */
    memcpy(wp+20,&wp_n,4);
    memcpy(wp+24,&wm_delete_window_atom,4);
    xreq(wp,28);

    regrid(cfg_w, cfg_h);   /* a ConfigureNotify may have beaten the atom replies */
    xup = 1;
    return 0;
}

int xterm_fd(void) { return xfd; }

int xterm_lost(void) { return xlost; }

/* Drop the connection now.  The server then destroys our window.  Used
 * before fork()ing a background process: it would otherwise inherit the
 * connection and keep the (by then dead) window on screen. */
void xterm_disconnect(void)
{
    if (xfd > 0) close(xfd);
    xfd = -1; xlost = 1;
}

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

static uint8_t redraw_pending;

/* ── Pointer ───────────────────────────────────────────────────────────
 * Pointer events are only asked of the server while the application wants
 * them (xterm_mouse_select), so a window that nobody is reporting the
 * mouse for gets none.  They are turned into cell coordinates and queued
 * for xterm_read_mouse, like keys. */
#define MOUSE_QUEUE_CAP 32
static XtermMouse mouse_queue[MOUSE_QUEUE_CAP];
static uint8_t    mouse_head, mouse_tail, mouse_level;
static int        mouse_last_col = -1, mouse_last_row = -1;

/* Multi-click detection: a press of the same button in the same cell as
 * the last one, within this many ms (X server time), counts on: 1, 2, 3,
 * then 1 again. */
#ifndef XTERM_MULTICLICK_MS
#define XTERM_MULTICLICK_MS 400
#endif
static uint32_t   click_t, click_cell;
static uint8_t    click_n, click_btn;

void xterm_mouse_select(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    if (level == mouse_level) return;
    mouse_level = (uint8_t)level;
    /* ButtonPress|ButtonRelease; ButtonMotion (1<<13: motion while a
     * button is held) or PointerMotion (1<<6: all motion). */
    uint32_t em = EMASK_BASE;
    if (level)      em |= (1u<<2) | (1u<<3);
    if (level == 2) em |= 1u<<13;
    if (level == 3) em |= 1u<<6;
    /* ChangeWindowAttributes (opcode 2), value-mask CWEventMask. */
    uint8_t r[16] = {0}; uint16_t len = 4; uint32_t vm = 0x800;
    r[0] = 2;
    memcpy(r+2, &len, 2); memcpy(r+4, &xterm_win, 4);
    memcpy(r+8, &vm, 4);  memcpy(r+12, &em, 4);
    xreq(r, 16);
    mouse_head = mouse_tail;          /* whatever was queued is stale now */
    mouse_last_col = mouse_last_row = -1;
}

/* ButtonPress (4), ButtonRelease (5) or MotionNotify (6): event-x/y at
 * offsets 24/26, modifier and button state at 28. */
static void mouse_event(int etype, const uint8_t *ev)
{
    if (!mouse_level) return;           /* in flight when it was turned off */
    int16_t x, y; uint16_t st;
    memcpy(&x, ev + 24, 2); memcpy(&y, ev + 26, 2); memcpy(&st, ev + 28, 2);
    /* While a button is held the pointer is grabbed, so it can be outside
     * the window (or its partial last cell): clamp to the grid. */
    int col = x < 0 ? 0 : x / cell_w, row = y < 0 ? 0 : y / cell_h;
    if (col >= cols) col = cols - 1;
    if (row >= rows) row = rows - 1;
    XtermMouse m;
    m.kind = (uint8_t)(etype - 4);
    m.col  = (uint16_t)col; m.row = (uint16_t)row;
    m.mods = (uint8_t)((st & 1 ? 4 : 0) | (st & 8 ? 8 : 0) | (st & 4 ? 16 : 0));
    m.clicks = 0;
    if (etype == 4 && ev[1] < 4) {
        uint32_t t, cell = (uint32_t)row << 16 | (uint32_t)col;
        memcpy(&t, ev + 4, 4);
        click_n = click_n && click_btn == ev[1] && cell == click_cell &&
                  t - click_t < XTERM_MULTICLICK_MS ? click_n % 3 + 1 : 1;
        click_t = t; click_cell = cell; click_btn = ev[1];
        m.clicks = click_n;
    }
    if (etype == 6) {
        /* Report motion only when it enters another cell. */
        if (col == mouse_last_col && row == mouse_last_row) return;
        m.button = (uint8_t)((st & 0x100) ? 1 : (st & 0x200) ? 2 : (st & 0x400) ? 3 : 0);
    } else {
        m.button = ev[1];
    }
    mouse_last_col = col; mouse_last_row = row;
    uint8_t next = (uint8_t)((mouse_tail + 1) % MOUSE_QUEUE_CAP);
    if (next == mouse_head) return;     /* full: drop */
    mouse_queue[mouse_tail] = m;
    mouse_tail = next;
}

int xterm_read_mouse(XtermMouse *m)
{
    if (mouse_head == mouse_tail) return 0;
    *m = mouse_queue[mouse_head];
    mouse_head = (uint8_t)((mouse_head + 1) % MOUSE_QUEUE_CAP);
    return 1;
}

/* ── Selections (the clipboard) ────────────────────────────────────────
 * We can own PRIMARY and CLIPBOARD, serving a UTF-8 text the application
 * keeps for us (xterm_selection_set), and ask the PRIMARY owner for its
 * text (xterm_paste_request).  Neither needs any buffer here: owned text
 * is the application's, and pasted text is handed over in small pieces as
 * it is read from the server.  Not supported: INCR transfers (very large
 * pastes from some clients) and any text format but UTF8_STRING. */
static const char *sel_text;
static size_t      sel_len;
static uint8_t     sel_owned;            /* bit 0: PRIMARY, bit 1: CLIPBOARD */
static void      (*paste_cb)(const uint8_t *, size_t, void *);
static void       *paste_ctx;

/* SetSelectionOwner (opcode 22), at the current time. */
static void set_owner(uint32_t sel, uint32_t owner)
{
    uint8_t r[16] = {0}; uint16_t len = 4;
    r[0] = 22; memcpy(r+2, &len, 2);
    memcpy(r+4, &owner, 4); memcpy(r+8, &sel, 4);
    xreq(r, 16);
}

void xterm_selection_set(const char *text, size_t len)
{
    sel_text = text; sel_len = len;
    sel_owned = text && a_clipboard ? 3 : 0;
    uint32_t owner = sel_owned ? xterm_win : 0;
    set_owner(1, owner);                 /* 1 is XA_PRIMARY */
    if (a_clipboard) set_owner(a_clipboard, owner);
}

/* ChangeProperty (opcode 18, mode Replace) of `units` items of `format`
 * bits on someone's window. */
static void change_property(uint32_t win, uint32_t prop, uint32_t type,
                            int format, const void *data, size_t units)
{
    size_t nbytes = units * (size_t)(format / 8), pad = -nbytes & 3;
    uint8_t r[24] = {0}; uint16_t len = (uint16_t)(6 + (nbytes + pad) / 4);
    uint32_t n = (uint32_t)units;
    static const uint8_t zeros[3];
    r[0] = 18; memcpy(r+2, &len, 2);
    memcpy(r+4, &win, 4); memcpy(r+8, &prop, 4); memcpy(r+12, &type, 4);
    r[16] = (uint8_t)format; memcpy(r+20, &n, 4);
    xreq(r, 24); xwrite(data, nbytes); xwrite(zeros, pad);
}

/* Another client wants our selection (event 30: time 4, owner 8,
 * requestor 12, selection 16, target 20, property 24).  Answer with a
 * property on its window and a SelectionNotify (via SendEvent, opcode 25);
 * property None in the notify means "can't". */
static void selection_request(const uint8_t *ev)
{
    uint32_t time, req, sel, target, prop;
    memcpy(&time, ev+4, 4);   memcpy(&req, ev+12, 4); memcpy(&sel, ev+16, 4);
    memcpy(&target, ev+20, 4); memcpy(&prop, ev+24, 4);
    if (!prop) prop = target;            /* old clients: property unset */
    uint8_t bit = sel == 1 ? 1 : (sel == a_clipboard ? 2 : 0);
    int ok = 0;
    if (bit & sel_owned) {
        if (target == a_targets) {
            uint32_t t[2] = { a_targets, a_utf8 };
            change_property(req, prop, 4 /* ATOM */, 32, t, 2); ok = 1;
        } else if (target == a_utf8) {
            size_t max = ((size_t)x_max_request_words - 6) * 4;
            change_property(req, prop, a_utf8, 8, sel_text, sel_len < max ? sel_len : max);
            ok = 1;
        }
    }
    uint8_t r[44] = {0}; uint16_t len = 11; uint32_t none = 0;
    r[0] = 25; memcpy(r+2, &len, 2); memcpy(r+4, &req, 4);
    uint8_t *e = r + 12;                 /* the SelectionNotify event */
    e[0] = 31; memcpy(e+4, &time, 4); memcpy(e+8, &req, 4);
    memcpy(e+12, &sel, 4); memcpy(e+16, &target, 4);
    memcpy(e+20, ok ? &prop : &none, 4);
    xreq(r, 44);
}

void xterm_paste_request(void (*cb)(const uint8_t *, size_t, void *), void *ctx)
{
    if (!a_utf8) { cb(NULL, 0, ctx); return; }
    paste_cb = cb; paste_ctx = ctx;
    /* ConvertSelection (opcode 24): PRIMARY as UTF8_STRING into our
     * window's private property; the answer is a SelectionNotify. */
    uint8_t r[24] = {0}; uint16_t len = 6; uint32_t prim = 1, none = 0;
    r[0] = 24; memcpy(r+2, &len, 2);
    memcpy(r+4, &xterm_win, 4); memcpy(r+8, &prim, 4);
    memcpy(r+12, &a_utf8, 4);   memcpy(r+16, &a_prop, 4);
    memcpy(r+20, &none, 4);
    xreq(r, 24);
}

/* The text has arrived in our property: pass it to cb, 4 KB of the
 * property per GetProperty (opcode 20), then delete it (opcode 19). */
static void fetch_paste(void (*cb)(const uint8_t *, size_t, void *), void *ctx)
{
    for (uint32_t off = 0; ; off += 1024) {
        uint8_t q[24] = {0}, r[32]; uint16_t len = 6;
        uint32_t lo = off, longs = 1024;
        q[0] = 20; memcpy(q+2, &len, 2);
        memcpy(q+4, &xterm_win, 4); memcpy(q+8, &a_prop, 4);   /* type: any */
        memcpy(q+16, &lo, 4); memcpy(q+20, &longs, 4);
        xreq(q, 24);
        if (!await_reply(r)) return;
        uint32_t extra, type, after, vlen;
        memcpy(&extra, r+4, 4); memcpy(&type, r+8, 4);
        memcpy(&after, r+12, 4); memcpy(&vlen, r+16, 4);
        /* Only plain 8-bit UTF8_STRING data (not, say, an INCR header). */
        size_t valid = (r[1] == 8 && type == a_utf8) ? vlen : 0, left = (size_t)extra * 4;
        uint8_t buf[512];
        while (left) {
            size_t n = left < sizeof buf ? left : sizeof buf;
            xread(buf, n);
            if (xlost) return;
            size_t give = valid < n ? valid : n;
            if (give) cb(buf, give, ctx);
            valid -= give; left -= n;
        }
        if (!after || type != a_utf8) break;
    }
    uint8_t d[12] = {0}; uint16_t dl = 3;
    d[0] = 19; memcpy(d+2, &dl, 2);
    memcpy(d+4, &xterm_win, 4); memcpy(d+8, &a_prop, 4);
    xreq(d, 12);
}

/* One 32-byte server event.  Key presses are queued; everything else only
 * records state for xterm_wait() to act on. */
static void handle_event(const uint8_t *ev)
{
    uint8_t etype = ev[0] & 0x7f;
    /* Bit 7 marks an event that another client sent with XSendEvent.  The
     * server delivers those to us like any other, so a program on the
     * display could type into the session, or press buttons in it, with no
     * input from the user.  Keys and buttons from such events are dropped,
     * as xterm does by default (allowSendEvents).  What a client sends by
     * protocol and the program depends on is still handled: a
     * SelectionNotify answering our request, a WM_DELETE_WINDOW from the
     * window manager, ConfigureNotify. */
    if ((ev[0] & 0x80) && (etype == 2 || etype == 4 || etype == 5 || etype == 6))
        return;     /* KeyPress, ButtonPress/Release, MotionNotify */
    if (etype == 12) {
        uint16_t count;
        memcpy(&count, ev + 16, 2);
        if (count == 0) { redraw_pending = 1; synced = 0; }
    } else if (etype == 22) {
        /* ConfigureNotify: new geometry (a window manager's resize, or
         * ours being moved).  Only the latest size matters, so just note
         * it: a drag produces dozens of these, and xterm_wait() regrids
         * once after reading them all. */
        uint16_t w, h;
        memcpy(&w, ev + 20, 2); memcpy(&h, ev + 22, 2);
        cfg_w = w; cfg_h = h;
    } else if (etype >= 4 && etype <= 6) {
        mouse_event(etype, ev);
    } else if (etype == 30) {
        selection_request(ev);
    } else if (etype == 29) {
        /* SelectionClear: someone else took the selection (selection
         * atom at 12). */
        uint32_t sel; memcpy(&sel, ev+12, 4);
        sel_owned &= (uint8_t)~(sel == 1 ? 1 : (sel == a_clipboard ? 2 : 0));
    } else if (etype == 31) {
        /* SelectionNotify, the answer to xterm_paste_request (property
         * at 20, None if the owner could not give us the text). */
        uint32_t prop; memcpy(&prop, ev+20, 4);
        void (*cb)(const uint8_t *, size_t, void *) = paste_cb;
        paste_cb = NULL;
        if (cb) { if (prop) fetch_paste(cb, paste_ctx); cb(NULL, 0, paste_ctx); }
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
}

/* Wait for X events.  Returns nonzero when everything must be repainted:
 * an expose series completed, or the window was resized -- in which case
 * xterm_columns()/xterm_rows() have changed and the cell buffers have
 * been reset, so the caller has to refill them before xterm_render(). */
int xterm_wait(int timeout_ms)
{
    if (xlost) return 0;
    struct pollfd p = { .fd = xfd, .events = POLLIN, .revents = 0 };
    long ready = poll(&p, 1, timeout_ms);
    if (ready <= 0) return 0;

    for (;;) {
        /* A dead connection can report POLLHUP/POLLERR without POLLIN;
         * reading is how we find out (see xread). */
        if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) break;
        uint8_t ev[32];
        xread(ev, sizeof ev);
        if (xlost) break;          /* (poll would go on saying "readable") */
        handle_event(ev);
        p.revents = 0;
        ready = poll(&p, 1, 0);
        if (ready <= 0) break;
    }
    if (font_delta) {                   /* Ctrl+= / Ctrl+- (see handle_keypress) */
        int k = font_size + font_delta;
        font_delta = 0;
        if (font_size_set(k)) redraw_pending = 1;
    }
    if (regrid(cfg_w, cfg_h)) redraw_pending = 1;
    int redraw = redraw_pending;
    redraw_pending = 0;
    return redraw;
}
