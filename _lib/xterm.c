/*
 * xterm.c — render-only X11 terminal surface
 *
 *   - Pure libc, zero X11 libraries
 *   - Raw X11 wire protocol over Unix socket
 *   - BFN4 bitmap font, one file per size: the glyphs and the colour emoji,
 *     with the codepoint of each in the file (produced by _font/)
 *   - File-backed font mapping; protocol buffers supplied by main's stack frame
 *   - One PutImage per tile of cells (no full-screen framebuffer)
 *
 * The font is loaded from /etc/bfnt/font2 (see BFN4 font).  xterm_init() creates the
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
#define EMASK_BASE ((1u<<21)|(1u<<17)|(1u<<15)|(1u<<0)|(1u<<4)) /* FocusChange|StructureNotify|Exposure|KeyPress|EnterWindow */

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
static void xkb_init(void);

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

/* What the keyboard table keeps of a key's symbol: 16 bits.  A character is
 * its code point (below 0xFE00); the keys that are not characters (Return,
 * the arrows, the keypad, ISO_Left_Tab, ...) keep their keysym, which is
 * 0xFE00-0xFFFF.  A keysym with no character and no use here is 0. */
static uint32_t keysym_cp(uint32_t ks);   /* the character a keysym types */

static uint16_t key_code(uint32_t ks)
{
    if (ks >= 0xFE00 && ks <= 0xFFFF) return (uint16_t)ks;
    uint32_t cp = keysym_cp(ks);
    return cp < 0xFE00 ? (uint16_t)cp : 0;
}

/* The table: for each keycode, each XKB layout group (XTERM_GROUPS of them;
 * the layouts of "setxkbmap -layout us,ru"), the symbol at 4 levels: base,
 * Shift, AltGr, AltGr+Shift.  All groups are loaded at start, so a key is
 * looked up in the group its event says, with no reload when the layout is
 * switched.  Indexed by keycode, 0 (nothing) where the keymap has no key. */
static uint8_t  min_keycode, max_keycode;
static uint16_t keymap[256][XTERM_GROUPS][4];

/* GetKeyboardMapping (opcode 101), for a server without XKB: the first four
 * columns of each keycode's row are taken as the four levels of the first
 * group, which is all the core protocol says.  The reply is streamed one row
 * at a time, whatever keysyms_per_keycode the server chose, so that no
 * configuration can overflow a buffer or throw off the wire format. */
static void load_keyboard_mapping(void)
{
    int count = (int)max_keycode - (int)min_keycode + 1;
    uint8_t req[8] = { 101, 0, 2, 0, min_keycode, (uint8_t)count };
    xreq(req, 8);

    uint8_t hdr[32];
    if (!await_reply(hdr)) return;
    for (int i = 0; i < count; i++)
        for (int col = 0; col < hdr[1]; col++) {
            uint32_t ks; xread(&ks, 4);
            if (col < 4) keymap[min_keycode + i][0][col] = key_code(ks);
        }
}

/* XKEYBOARD major opcode, 0 if the server has no XKB or setup failed. */
static uint8_t xkb_major;

/* The level each key type picks, for c = Shift + 2 * AltGr (see
 * xkb_load_keymap); the types are numbered as the server has them. */
#define XKB_TYPES_MAX 32
static uint8_t xkb_level[XKB_TYPES_MAX][4];

/* XkbGetMap (minor 8), for the real thing.  The reply has the key types
 * first, so that AltGr is the level the keymap says (a key type maps
 * modifiers to levels; it is not always the third column), then the symbols
 * of every key, group after group.  Returns 0 if the server will not say.
 * Wire layout (xcb/xkb.h): 40 bytes of fixed header, then the types (an
 * 8-byte record, its 8-byte map entries, and 4-byte preserve entries if it
 * has any), then per key an 8-byte record followed by nSyms keysyms. */
static int xkb_load_keymap(void)
{
    int count = (int)max_keycode - (int)min_keycode + 1;
    /* XkbGetMap, 28 bytes: the core keyboard; all key types ("full"), the
     * symbols of the keycode range ("partial"). */
    uint8_t req[28] = { xkb_major, 8, 7, 0, 0, 1, 1, 0, 2, 0, 0, 0, min_keycode, (uint8_t)count };
    xreq(req, sizeof req);
    uint8_t hdr[32];
    if (!await_reply(hdr)) return 0;
    xdrain(8);                                       /* the rest of the fixed header */

    for (int t = 0; t < hdr[15]; t++) {              /* nTypes */
        uint8_t th[8], me[8], spare[4];
        xread(th, 8);                                /* mask, ..., nMapEntries, hasPreserve */
        uint8_t *lv = t < XKB_TYPES_MAX ? xkb_level[t] : spare;
        for (int e = 0; e < th[5]; e++) {
            xread(me, 8);                            /* active, mods, level, ... */
            for (int c = 0; c < 4; c++) {
                /* The modifiers the column stands for: Shift (Mod bit 0), or
                 * for a type that selects on NumLock (the keypad's) NumLock
                 * (Mod2), whose second level is the digit, as in the core
                 * table; and AltGr, taken to be Mod5 as in handle_keypress. */
                uint8_t m = (uint8_t)(((c & 1) ? (th[0] & 0x10 ? 0x10 : 1) : 0) | ((c & 2) << 6));
                if (me[0] && me[1] == (m & th[0])) lv[c] = me[2];
            }
        }
        if (th[6]) xdrain((size_t)th[5] * 4);
    }

    for (int i = 0; i < count; i++) {
        uint8_t kh[8];
        xread(kh, 8);                    /* key type of each group, groupInfo, width, nSyms */
        uint8_t ng = kh[4] & 0x0F, width = kh[5];
        uint16_t nsyms; memcpy(&nsyms, kh+6, 2);
        uint16_t take = nsyms < 32 ? nsyms : 32;
        uint32_t s[32];
        xread(s, (size_t)take * 4);      /* group after group, width levels each */
        xdrain((size_t)(nsyms - take) * 4);
        for (int g = 0; g < XTERM_GROUPS && ng; g++) {
            uint8_t gi = g % ng;         /* fewer groups than we hold: wrap */
            uint8_t kt = kh[gi];
            for (int c = 0; c < 4; c++) {
                uint8_t lvl = kt < XKB_TYPES_MAX ? xkb_level[kt][c] : 0;
                unsigned at = (unsigned)gi * width + lvl;
                keymap[min_keycode + i][g][c] = (lvl < width && at < take) ? key_code(s[at]) : 0;
            }
        }
    }
    return 1;
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
static uint32_t keysym_of(const uint16_t *row, int shift, int altgr, int numlock, int capslock)
{
    uint32_t base = row[0];
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
    uint32_t ks = row[lvl];
    if (!ks && (lvl & 1)) ks = row[lvl & ~1];   /* shift level empty */
    if (!ks && (lvl & 2)) ks = row[0];          /* AltGr level empty */
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

/* Keysyms that are not their code point, in pairs (keysym, code point): the
 * characters us-intl types beyond Latin-1 (AltGr+5 is EuroSign, AltGr+9 and
 * AltGr+0 the single quotes, and so on). */
static const uint16_t cp_pairs[] = {
    0x20ac, 0x20ac,   /* EuroSign */
    0x0ac9, 0x2122,   /* trademark */
    0x0ad0, 0x2018,   /* leftsinglequotemark */
    0x0ad1, 0x2019,   /* rightsinglequotemark */
    0x0ad2, 0x201c,   /* leftdoublequotemark */
    0x0ad3, 0x201d,   /* rightdoublequotemark */
    0x13bc, 0x0152,   /* OE */
    0x13bd, 0x0153,   /* oe */
#if XTERM_LATIN2
    0x02a9, 0x0130,   /* Iabovedot: capital dotted I (the Turkish letters of Latin-3) */
    0x02ab, 0x011e,   /* Gbreve */
    0x02b9, 0x0131,   /* idotless: small dotless i */
    0x02bb, 0x011f,   /* gbreve */
#endif
};

#if XTERM_GREEK
/* Greek letters (X11 keysyms 0x7c1-0x7f9, without the accented ones) to their
 * code points.  The capitals run on from ALPHA, skipping the unassigned
 * U+03A2; the small letters run on from alpha, except that the keysyms put
 * sigma (0x7f2) before the final sigma (0x7f3) and Unicode the other way. */
static uint32_t greek_cp(uint32_t ks)
{
    if (ks >= 0x7c1 && ks <= 0x7d8) return 0x391 + (ks - 0x7c1) + (ks >= 0x7d2);
    if (ks >= 0x7e1 && ks <= 0x7f9) return (0x3b1 + (ks - 0x7e1)) ^ (ks == 0x7f2 || ks == 0x7f3);
    return 0;
}
#endif

#if XTERM_LATIN2
/* Latin-2 letters (keysyms 0x1a1-0x1ff) to their code points, 0 for the
 * unassigned slots; from the U+ comments in keysymdef.h.  The table runs from
 * 0x1a1 to 0x1df, the capitals; the small letters of 0x1e0-0x1fe are the
 * capitals of 0x1c0-0x1de plus one, and 0x1ff is the dot above. */
static const uint16_t lat2_tab[] = {
    0x0104, 0x02d8, 0x0141, 0x0000, 0x013d, 0x015a, 0x0000, 0x0000,
    0x0160, 0x015e, 0x0164, 0x0179, 0x0000, 0x017d, 0x017b, 0x0000,
    0x0105, 0x02db, 0x0142, 0x0000, 0x013e, 0x015b, 0x02c7, 0x0000,
    0x0161, 0x015f, 0x0165, 0x017a, 0x02dd, 0x017e, 0x017c, 0x0154,
    0x0000, 0x0000, 0x0102, 0x0000, 0x0139, 0x0106, 0x0000, 0x010c,
    0x0000, 0x0118, 0x0000, 0x011a, 0x0000, 0x0000, 0x010e, 0x0110,
    0x0143, 0x0147, 0x0000, 0x0000, 0x0150, 0x0000, 0x0000, 0x0158,
    0x016e, 0x0000, 0x0170, 0x0000, 0x0000, 0x0162, 0x0000,
};

static uint32_t latin2_cp(uint32_t ks)
{
    if (ks < 0x1a1) return 0;                 /* ks < 0x200: see keysym_cp */
    if (ks == 0x1ff) return 0x2d9;
    if (ks < 0x1e0) return lat2_tab[ks - 0x1a1];
    uint32_t cp = lat2_tab[ks - 0x1c1];
    return cp ? cp + 1 : 0;
}
#endif

#if XTERM_CYRILLIC
/* Cyrillic letters (keysyms 0x6a1-0x6ff) to their code points, from the U+
 * comments in keysymdef.h (every slot is assigned).  The table runs from
 * 0x6a1 to 0x6df; the capitals of 0x6e0-0x6ff are the small letters of
 * 0x6c0-0x6df less 0x20. */
static const uint16_t cyr_tab[] = {
    0x0452, 0x0453, 0x0451, 0x0454, 0x0455, 0x0456, 0x0457, 0x0458,
    0x0459, 0x045a, 0x045b, 0x045c, 0x0491, 0x045e, 0x045f, 0x2116,
    0x0402, 0x0403, 0x0401, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408,
    0x0409, 0x040a, 0x040b, 0x040c, 0x0490, 0x040e, 0x040f, 0x044e,
    0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445,
    0x0438, 0x0439, 0x043a, 0x043b, 0x043c, 0x043d, 0x043e, 0x043f,
    0x044f, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432, 0x044c,
    0x044b, 0x0437, 0x0448, 0x044d, 0x0449, 0x0447, 0x044a,
};

static uint32_t cyrillic_cp(uint32_t ks)
{
    if (ks < 0x6a1) return 0;                 /* ks < 0x700: see keysym_cp */
    if (ks < 0x6e0) return cyr_tab[ks - 0x6a1];
    return cyr_tab[ks - 0x6c1] - 0x20;
}
#endif

/* The code point of a keysym that types a character, 0 if it types none.
 * X11's Latin-1 keysyms (0x20-0x7e, 0xA0-0xFF) are the code points.  So are
 * the Unicode keysyms, 0x01000000 plus the code point: how the XKB files give
 * the characters that have no legacy keysym (ro's comma-below letters, de's
 * sharp capital, ua's hryvnia, ...). */
static uint32_t keysym_cp(uint32_t ks)
{
    if ((ks & 0xFF000000) == 0x01000000) {
        ks &= 0xFFFFFF;
        /* not a control character, a surrogate, or one of the UTF-8 that a
         * keyboard table of 16 bits (see key_code) has no room for */
        return (ks - 0x20 < 0x5F || ks - 0xA0 < 0xD760 || ks - 0xE000 < 0x1E00) ? ks : 0;
    }
    if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xA0 && ks <= 0xFF)) return ks;
    for (unsigned i = 0; i < sizeof cp_pairs / sizeof *cp_pairs; i += 2)
        if (ks == cp_pairs[i]) return cp_pairs[i + 1];
#if XTERM_GREEK
    if (ks >> 8 == 7) return greek_cp(ks);
#endif
#if XTERM_LATIN2
    if (ks >> 8 == 1) return latin2_cp(ks);
#endif
#if XTERM_CYRILLIC
    if (ks >> 8 == 6) return cyrillic_cp(ks);
#endif
    return 0;
}

/* The bytes a typed key sends: characters (ASCII as-is, the rest
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

    if (ks >= 0x20 && ks < 0xFE00) {            /* a character: UTF-8 */
        if (ks < 0x80) {
            o->b[o->n++] = (uint8_t)ks;
        } else if (ks < 0x800) {
            o->b[o->n++] = (uint8_t)(0xC0 | (ks >> 6));
            o->b[o->n++] = (uint8_t)(0x80 | (ks & 0x3F));
        } else {
            o->b[o->n++] = (uint8_t)(0xE0 | (ks >> 12));
            o->b[o->n++] = (uint8_t)(0x80 | ((ks >> 6) & 0x3F));
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
 *   - typed keys (key_bytes): characters (ASCII as-is, the rest
 *     UTF-8-encoded, as everything downstream expects on the wire), Ctrl-
 *     letter control codes, and the few control keys.  An Alt ESC prefix
 *     goes only before a key that sends something.
 * Anything else is silently ignored.
 *
 * What keysym_of() returns is the table's 16-bit key code (see key_code): a
 * character is its code point, any other key its keysym. */
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
    uint16_t state; memcpy(&state, ev+28, 2);
    /* The layout group is in bits 13-14 of the state. */
    const uint16_t *row = keymap[ev[1]][(state >> 13) & (XTERM_GROUPS - 1)];
    int shift = (state & 0x0001) != 0;
    int ctrl  = (state & 0x0004) != 0;
    int alt   = (state & 0x0008) != 0;  /* Mod1: conventional "Alt" binding */
    int capslock = XTERM_CAPSLOCK && (state & 0x0002) != 0;  /* Lock: see keysym_of */
    int numlock = (state & 0x0010) != 0;  /* Mod2: NumLock, see keysym_of */
    int altgr = (state & 0x0080) != 0;  /* Mod5: conventional AltGr /
                                          * ISO_Level3_Shift binding, as
                                          * produced by e.g. xkb option
                                          * "lv3:ralt_switch" */

    uint32_t ks = keysym_of(row, shift, altgr, numlock, capslock);
    /* Control is about Latin letters: in a group of Cyrillic or Greek ones,
     * Ctrl+<the key> is Ctrl+<the letter the first group has on it>, for the
     * control codes and for the shortcuts below alike (Ctrl+V pastes). */
    uint16_t latin = keymap[ev[1]][0][0];
    if (ctrl && ks >= 0x80 && ks < 0xFE00 && latin > 0x20 && latin < 0x7F) ks = latin;
    /* Insert usually has nothing in its shifted column, so Shift+Insert has
     * to be recognized by the unshifted one. */
    int insert = ks == 0xFF63 ||
                 (shift && row[altgr ? 2 : 0] == 0xFF63);
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
    if (is_keypad(row[0])) { ctrl = 0; alt = 0; }

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
 * BFN4 font
 * ═══════════════════════════════════════════════════════════════════ */

#define GRID_MAX_CELLS (32 * 1024)
#define SETUP_MAX      (8 * 1024)
/* The bytes of one tile of cells, see cell_storage and render_all(). */
#define TILE_BYTES     (PUT_HDR + 64*64*4)

/* The font comes in FONT_SIZES sizes (see _font/mkfont.c), one file for each:
 * /etc/bfnt/font<k> for size k, holding its glyphs and its colour emoji.  Size
 * k's cell is 2^((k - BASE_SIZE)/4) times the base size's in both directions.
 * The font starts at BASE_SIZE.  The cell size is in the file's header, so the
 * name only says which size the file is. */
#define FONT_SIZES     8
#define BASE_SIZE      2
#define BFNT_DIR       "/etc/bfnt/"
#define FONT_HDR       18     /* bytes before the tables: magic, cell, counts, languages */
/* Narrow glyph slots 0..FIXED_N-1 are U+0020..U+007E (slots 0..94) and
 * U+00A0..U+00FF (slots 95..190) in every font (see _font/charset.h): a
 * character in those ranges needs no table lookup. */
#define LATIN1_SLOT    95
#define FIXED_N        (LATIN1_SLOT + 96)

static uint8_t  *font_data;     /* the font in use, mapped; NULL until one is loaded */
static size_t    font_len;
static uint16_t  cell_w, cell_h, baseline, num_glyphs, num_wide, num_emoji;
static uint16_t  font_langs;    /* the XTERM_LANGS bits the font has glyphs for */
static const uint8_t *ncp_tab;  /* codepoints (u32) of the narrow glyphs past ASCII and Latin-1 */
static const uint8_t *wcp_tab;  /* codepoints (u32) of the wide glyphs */
static const uint8_t *emoji_cps;   /* codepoints (u32) of the emoji */
static const uint8_t *glyph_data;
static const uint8_t *wide_data;   /* num_wide glyphs of 2*cell_w x cell_h, after the narrow ones */
static const uint8_t *emoji_data;  /* num_emoji records, after the emoji's codepoints (see draw_emoji) */
static size_t    emoji_rec;     /* bytes per record: 45 palette + packed 4bpp pixels */
static int       WIN_W, WIN_H, cols, rows;
static int       cfg_w, cfg_h;  /* window size as last reported by the server */
static int       font_size = BASE_SIZE;  /* the size in use */
static char      font_path[]  = BFNT_DIR "font2";     /* the digit is the size: */
#define FONT_DIGIT  (sizeof BFNT_DIR "font" - 1)     /* see font_switch() */

#ifndef SEEK_END
#define SEEK_END 2
#endif

/* A 16-bit and a 32-bit little-endian field of a BFN4 file. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p+2) << 16; }

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

/* Whether m, a mapping of n bytes, is a font that can be drawn: the header is
 * right, and everything it counts is in the file -- the codepoint tables, the
 * glyphs, and the emoji section.  A font that passes is one the renderer can
 * use, so nothing is read past the mapping. */
static int font_valid(const uint8_t *m, size_t n)
{
    if (n < FONT_HDR || m[0] != 'B' || m[1] != 'F' || m[2] != 'N' || m[3] != '4')
        return 0;
    size_t w = rd16(m+4), h = rd16(m+6);
    size_t ng = rd16(m+10), nw = rd16(m+12), ne = rd16(m+14);
    if (w == 0 || h == 0) return 0;
    /* ASCII and Latin-1, then the table's narrow glyphs, up to the 511 slots
     * (0..510) that 9 bits name besides vt.c's wide-character mark; the wide
     * ones within the 256 a byte names; the emoji too (255 is the "none"
     * index) */
    if (ng < FIXED_N || ng > 511 || nw > 256 || ne > 255) return 0;
    /* the bitmaps: (n-FONT_HDR) / h / w is the number of cells the file has
     * room for, with no overflow -- checked before anything multiplies */
    if ((n - FONT_HDR) / h / w < ng + 2 * nw) return 0;
    /* the codepoint tables of the narrow and wide glyphs, 4 bytes each */
    if (4 * (ng - FIXED_N + nw) > n - FONT_HDR - w * h * (ng + 2 * nw)) return 0;
    size_t rest = n - FONT_HDR - w * h * (ng + 2 * nw) - 4 * (ng - FIXED_N + nw);
    /* the emoji: a codepoint (4 bytes) and a record of 45 bytes of palette and
     * the w*h bytes of 4bpp pixels (two cells wide) each */
    if (ne > rest / (49 + w * h)) return 0;
    /* a cell fits a tile at the widest depth, with the tile's padding (tile_fit) */
    return (TILE_BYTES - PUT_HDR) / h >= 3 + w * 4;
}

/* Map and check the font at path, without changing the font in use: the
 * caller commits it with font_commit(), or unmaps it. */
static uint8_t *font_open(const char *path, size_t *len)
{
    uint8_t *m = map_file(path, len);
    if (!m) return NULL;
    if (!font_valid(m, *len)) {
        munmap(m, *len);
        return NULL;
    }
    return m;
}

/* Make m, from font_open(), the font in use; the one it replaces is unmapped. */
static void font_commit(uint8_t *m, size_t len)
{
    if (font_data) munmap(font_data, font_len);
    font_data = m; font_len = len;
    cell_w = rd16(m+4); cell_h = rd16(m+6); baseline = rd16(m+8);
    num_glyphs = rd16(m+10); num_wide = rd16(m+12); num_emoji = rd16(m+14);
    font_langs = rd16(m+16);
    ncp_tab    = m + FONT_HDR;
    wcp_tab    = ncp_tab + 4 * (size_t)(num_glyphs - FIXED_N);
    glyph_data = wcp_tab + 4 * (size_t)num_wide;
    wide_data  = glyph_data + (size_t)cell_w * cell_h * num_glyphs;
    emoji_cps  = wide_data + (size_t)cell_w * 2 * cell_h * num_wide;
    emoji_data = emoji_cps + 4 * (size_t)num_emoji;
    emoji_rec  = 45 + ((size_t)cell_w * 2 * cell_h + 1) / 2;
}

/* The glyph lookups.  A character is drawn with the glyph of its codepoint in
 * the font in use, if the font has one: these say which slot that is (narrow,
 * wide, or emoji), or -1 if there is none, when the caller draws a placeholder
 * box instead.  Which codepoints are double-width is not the font's (width.h). */
int xterm_glyph_slot(uint32_t cp)
{
    if (!font_data) return -1;
    if (cp >= 0x20 && cp <= 0x7E) return (int)(cp - 0x20);
    if (cp >= 0xA0 && cp <= 0xFF) return LATIN1_SLOT + (int)(cp - 0xA0);
    for (int i = 0; i < num_glyphs - FIXED_N; i++)
        if (rd32(ncp_tab + 4 * (size_t)i) == cp) return FIXED_N + i;
    return -1;
}

int xterm_wide_slot(uint32_t cp)
{
    if (!font_data) return -1;
    for (int i = 0; i < num_wide; i++)
        if (rd32(wcp_tab + 4 * (size_t)i) == cp) return i;
    return -1;
}

/* The emoji index of codepoint cp, the record of the font's emoji section that
 * draws it, or -1 if this font has no emoji for it.  A font without emoji (or
 * whose emoji section did not load, which rejects the font) draws them as
 * placeholder boxes (see draw_box). */
int xterm_emoji_index(uint32_t cp)
{
    if (!font_data) return -1;
    for (int i = 0; i < num_emoji; i++)
        if (rd32(emoji_cps + 4 * (size_t)i) == cp) return i;
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Terminal state
 * ═══════════════════════════════════════════════════════════════════ */

/* The per-cell planes -- glyph slot, fg, bg and attributes, a byte each --
 * live in one anonymous mapping, sized for the grid in use (4 bytes a cell),
 * so no memory is set aside for a grid larger than the window.  GRID_MAX_CELLS
 * only bounds how large a grid may get. */
static uint8_t *scr;
static uint8_t *fgb, *bgb, *atb;
static uint8_t *planes;
static size_t   planes_len;

/* Make a grid of (screen_w / cell_w) x (screen_h / cell_h) cells the current
 * one, with fresh planes: blank glyphs, default colours, no attributes.
 * Returns 0, and changes nothing, if the grid has no cell or is too large for
 * the limit, or the planes cannot be mapped. */
static int grid_init(uint16_t screen_w, uint16_t screen_h)
{
    int c = screen_w / cell_w, r = screen_h / cell_h;
    if (c < 1 || r < 1 || (size_t)c * r > GRID_MAX_CELLS) return 0;
    size_t cells = (size_t)c * r;
    uint8_t *m = mmap(0, 4 * cells, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 0;
    if (planes) munmap(planes, planes_len);
    planes = m; planes_len = 4 * cells;
    cols = c; rows = r;
    WIN_W = cols * cell_w;
    WIN_H = rows * cell_h;
    /* a fresh mapping is all zero: that is the blank glyph, bg slot 0 (which
     * is XTERM_BG_DEFAULT) and no attributes; only the fg needs filling */
    scr = m; bgb = m + cells; atb = m + 2 * cells; fgb = m + 3 * cells;
    memset(fgb, XTERM_FG_DEFAULT, cells);
    return 1;
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
    if (k >= num_emoji) { draw_box(half * cell_w, cell_w * 2, fg, depth); return; }
    const uint8_t *rec = emoji_data + (size_t)k * emoji_rec;
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
    /* slot: 256 more than the glyph byte, for a narrow cell flagged HI */
    int slot = scr[idx];
    if ((rattrs & XTERM_ATTR_HI) == XTERM_ATTR_HI) { slot += 256; rattrs &= (uint8_t)~XTERM_ATTR_HI; }
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
    int cp = slot;
    /* Anything that isn't an ordinary glyph (emoji, placeholder box) is
     * drawn over a plain background, which is what glyph slot 0 gives:
     * it is the space (slot 0), so its coverage is all zero. */
    const uint8_t *g = glyph_data;
    int sh = 0, stride = cell_w;
    if (!is_emoji && !(rattrs & XTERM_ATTR_MISSING) && (rattrs & XTERM_ATTR_WIDE)) {
        /* A wide glyph: this cell's half of a two-cell bitmap. An index
         * past the font's wide glyphs leaves g on slot 0, a blank. */
        if (cp < num_wide) {
            g = wide_data + (size_t)cp * 2 * cell_w * cell_h + (size_t)half * cell_w;
            stride = 2 * cell_w;
        }
    } else if (!is_emoji && !(rattrs & XTERM_ATTR_MISSING) && cp < num_glyphs) {
        g += (size_t)cp*cell_w*cell_h;
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
    if ((size_t)c * r > GRID_MAX_CELLS) r = GRID_MAX_CELLS / c;
    if (c == cols && r == rows) return 0;
    if (!grid_init((uint16_t)(c * cell_w), (uint16_t)(r * cell_h)))
        return 0;                       /* out of memory: the old grid stays */
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
 * returned.  The emoji are in the font, so they change size with it.  The
 * caller re-derives the grid from the window. */
static int font_switch(int k, int win_w, int win_h)
{
    if (k == font_size) return 0;
    font_path[FONT_DIGIT] = (char)('0' + k);
    size_t len;
    uint8_t *m = font_open(font_path, &len);
    if (m && !grid_fits(rd16(m+4), rd16(m+6), win_w, win_h)) { munmap(m, len); m = NULL; }
    if (!m) {
        font_path[FONT_DIGIT] = (char)('0' + font_size);
        return 0;
    }
    font_commit(m, len);
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
    if (!fm) die("could not load a BFN4 font: /etc/bfnt/font2 (make -C _font install)\n");
    font_commit(fm, flen);
    /* A keyboard language this program has (xterm_conf.h) needs its glyphs in
     * the font: without them its characters would come out as boxes, so refuse.
     * A font with more languages than the program is fine. */
    if (XTERM_LANGS & ~(unsigned)font_langs)
        die("the font in /etc/bfnt lacks the glyphs of a keyboard language in xterm_conf.h:\n"
            "make -C _font install (after making the fonts with the same xterm_conf.h)\n");

    /* Close-on-exec (SOCK_CLOEXEC has O_CLOEXEC's value): the shells and
     * programs we start must not inherit the connection.  They would keep
     * it, and so the window, alive after we have detached or exited. */
    /* $DISPLAY is ":N" or ":NN" (optionally ".screen", which we don't need:
     * only the display number selects the socket).  N is copied straight out
     * of the environment string.  Falls back to display 2 if $DISPLAY is unset
     * or doesn't look like ":<digits>". */
    char num[3] = { '2', 0, 0 };
#ifdef X11_BACKEND
    const char *disp = getenv("DISPLAY");
#else
    const char *disp = NULL;  /* standalone demo: no libc, no $DISPLAY */
#endif
    if (disp && disp[0] == ':' && disp[1] >= '0' && disp[1] <= '9') {
        num[0] = disp[1];
        if (disp[2] >= '0' && disp[2] <= '9') num[1] = disp[2];
    }
    /* The server's socket is in /tmp/.X11-unix on most systems and in
     * /run/.X11-unix on others: try both, a fresh socket for each. */
    static const char *const dirs[2] = { "/tmp/.X11-unix/X", "/run/.X11-unix/X" };
    struct sockaddr_un sa;
    int connected = 0;
    for (unsigned k = 0; k < 2 && !connected; k++) {
        xfd = socket(AF_UNIX, SOCK_STREAM | O_CLOEXEC, 0);
        sa.sun_family = AF_UNIX;
        strlcpy(sa.sun_path, dirs[k], sizeof sa.sun_path);
        memcpy(sa.sun_path + strlen(sa.sun_path), num, sizeof num);
        int sa_len = (int)(sizeof sa.sun_family + strlen(sa.sun_path) + 1);
        connected = connect(xfd, (struct sockaddr *)&sa, sa_len) == 0;
        if (!connected) close(xfd);
    }
    if (!connected)
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
    xkb_init();
    if (!xkb_major || !xkb_load_keymap()) load_keyboard_mapping();
    if (!grid_init(screen_w, screen_h)) die("terminal grid is too large or too small\n");

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

/* ── Bell ────────────────────────────────────────────────────────────
 * A BEL asks the window manager for the window's attention: the ICCCM urgency
 * hint in WM_HINTS (4.1.2.4), which taskbars and docks mark.  Only while the
 * window lacks the keyboard focus, and taken back when the focus comes: ICCCM
 * requires the client to clear it.  InputHint is left unset, so the window
 * manager's focus policy is unchanged. */
static uint8_t has_focus, urgent;

static void set_urgency(uint32_t flags)
{
    uint32_t h[9] = { flags };     /* WM_HINTS: flags, then nine CARD32 fields, rest 0 */
    change_property(xterm_win, 35, 35, 32, h, 9);   /* atoms WM_HINTS (35), type WM_HINTS */
}

/* QueryExtension("XKEYBOARD") and XkbUseExtension 1.0, which the server
 * requires before any other XKB request. */
static void xkb_init(void)
{
    uint8_t q[20] = { 98, 0, 5, 0, 9, 0, 0, 0 };       /* QueryExtension, 9-char name */
    memcpy(q + 8, "XKEYBOARD", 9);
    xreq(q, sizeof q);
    uint8_t r[32];
    if (!await_reply(r) || !r[8]) return;               /* not present */
    uint8_t major = r[9];
    uint8_t use[8] = { major, 0, 2, 0, 1, 0, 0, 0 };    /* XkbUseExtension, 1.0 */
    xreq(use, sizeof use);
    if (!await_reply(r) || !r[1]) return;               /* not supported */
    xkb_major = major;
}

/* XkbBell with eventOnly set: the server reports the bell to clients that
 * select BellNotify, and makes no sound.  Sent on every BEL, focused or not:
 * a window manager only sees the bell of an unfocused window (through the
 * urgency hint), but a client that listens for bell notifications sees both. */
static void xkb_bell_notify(void)
{
    if (!xkb_major) return;
    uint8_t b[28] = { 0 };
    b[0] = xkb_major; b[1] = 3; b[2] = 7;               /* X_kbBell, 7 words */
    b[5] = 1;                                           /* deviceSpec: XkbUseCoreKbd (0x0100) */
    b[7] = 3;                                           /* bellClass: XkbDfltXIClass (0x0300) */
    b[9] = 4;                                           /* bellID:    XkbDfltXIId (0x0400) */
    b[12] = 1;                                          /* eventOnly */
    memcpy(b + 24, &xterm_win, 4);                      /* window */
    xreq(b, sizeof b);
}

void xterm_bell(void)
{
    xkb_bell_notify();
    if (has_focus || urgent) return;
    urgent = 1;
    set_urgency(1u << 8);          /* UrgencyHint */
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
    } else if (etype == 9 || etype == 10) {
        /* FocusIn / FocusOut.  Coming to the window takes back its attention
         * mark (see xterm_bell). */
        has_focus = etype == 9;
        if (has_focus && urgent) { urgent = 0; set_urgency(0); }
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
