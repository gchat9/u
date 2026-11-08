/* input.h - keys: decoding what the terminal sends, the line editor used by
 * every input field, and the block of input that the main loop and the
 * questions of a job both take their keys from. */

/* ----------------------------------------------------------- keys and lines */

#define K_INCOMPLETE (-1)
#define K_ESC    0x1b
#define K_UP     0x100
#define K_DOWN   0x101
#define K_LEFT   0x102
#define K_RIGHT  0x103
#define K_HOME   0x104
#define K_END    0x105
#define K_PGUP   0x106
#define K_PGDN   0x107
#define K_DEL    0x108
#define K_F3     0x109
#define K_F10    0x10a
#define K_CLEFT  0x10b
#define K_CRIGHT 0x10c
#define K_ALTBS  0x10d
#define K_CHAR   0x100000   /* a character typed: K_CHAR | its code point */
#define K_F8     0x10e
#define K_F5     0x10f
#define K_F6     0x110
#define K_SUP    0x111
#define K_SDOWN  0x112
#define K_INS    0x113
#define K_F9     0x114
#define K_BTAB   0x115
#define K_NONE   0x1ff
#define K_ALT    0x200          /* Alt+x is K_ALT | x */

/* The key for the bytes at in[i], which is not an Esc: a well-formed UTF-8 character
 * is one key, K_CHAR | its code point, of *len bytes; anything else is its byte, as
 * before. K_INCOMPLETE: a character the end of the input cuts off. */
static int char_key(const unsigned char *in, int n, int i, int *len)
{
    int c = in[i], l, k;
    uint32_t cp;

    if (c < 0xc2 || c > 0xf4) return c;
    l = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
    if (i + l > n) return K_INCOMPLETE;
    cp = c & ((1 << (7 - l)) - 1);
    for (k = 1; k < l; k++) {
        if ((in[i + k] & 0xc0) != 0x80) return c;
        cp = cp << 6 | (in[i + k] & 0x3f);
    }
    if (cp < (l == 2 ? 0x80 : l == 3 ? 0x800 : 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return c;
    *len = l;
    return K_CHAR | (int)cp;
}

/* Decode one key at in[i]. Returns the key and sets *len, or K_INCOMPLETE if
 * the buffer ends inside a key. An Esc starts a sequence: Alt+x, or a CSI or SS3 key. */
static int key_parse(const unsigned char *in, int n, int i, int *len)
{
    int j, k, fin, c, nums[2];

    *len = 1;
    if (in[i] != K_ESC) return char_key(in, n, i, len);
    if (i + 1 >= n) return K_INCOMPLETE;
    if (in[i + 1] == 0x7f || in[i + 1] == 0x08) { *len = 2; return K_ALTBS; }
    if (in[i + 1] == K_ESC) return K_ESC;       /* ESC ESC: the first is Esc, as in mc */
    if (in[i + 1] >= 0x80) return K_ESC;        /* not Alt+a character: the Esc alone */
    if (in[i + 1] != '[' && in[i + 1] != 'O') { *len = 2; return K_ALT | (in[i + 1] & 0x7f); }

    j = i + 2;
    nums[0] = nums[1] = -1;
    k = 0;
    while (j < n && in[j] >= 0x20 && in[j] < 0x40) {   /* parameters and intermediates */
        c = in[j];
        if (c >= '0' && c <= '9') nums[k] = (nums[k] < 0 ? 0 : nums[k]) * 10 + (c - '0');
        else if (c == ';' && k == 0) k = 1;
        j++;
    }
    if (j >= n) return K_INCOMPLETE;
    fin = in[j];
    *len = j + 1 - i;
    switch (fin) {
    case 'A': return nums[1] == 2 ? K_SUP : K_UP;
    case 'B': return nums[1] == 2 ? K_SDOWN : K_DOWN;
    case 'C': return nums[1] == 5 ? K_CRIGHT : K_RIGHT;
    case 'D': return nums[1] == 5 ? K_CLEFT : K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'R': return in[i + 1] == 'O' ? K_F3 : K_NONE;
    case 'Z': return K_BTAB;
    case '~':
        switch (nums[0]) {
        case 1: case 7:  return K_HOME;
        case 4: case 8:  return K_END;
        case 2:          return K_INS;
        case 3:          return K_DEL;
        case 5:          return K_PGUP;
        case 6:          return K_PGDN;
        case 13:         return K_F3;
        case 15:         return K_F5;
        case 17:         return K_F6;
        case 19:         return K_F8;
        case 20:         return K_F9;
        case 21:         return K_F10;
        }
        return K_NONE;
    }
    return K_NONE;
}

/* Wait up to MS milliseconds for more input, appending it to BUF after the N
 * bytes already there. Returns the new count. */
static int wait_input(unsigned char *buf, int n, int cap, int ms)
{
    struct pollfd pf;
    long r;

    if (n >= cap) return n;
    pf.fd = 0;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, ms) > 0) {
        r = read(0, buf + n, (size_t)(cap - n));
        if (r > 0) n += (int)r;
    }
    return n;
}

/* Where the character before P starts, and where the one at P ends. */
static int char_prev(const struct line *l, int p)
{
    if (p > 0) p--;
    while (p > 0 && ((unsigned char)l->buf[p] & 0xc0) == 0x80) p--;
    return p;
}

static int char_next(const struct line *l, int p)
{
    if (p < l->len) p++;
    while (p < l->len && ((unsigned char)l->buf[p] & 0xc0) == 0x80) p++;
    return p;
}

/* The character boundary at most CELLS cells before P in L. */
static int back_cells(const struct line *l, int p, int cells)
{
    int c = 0;

    while (p > 0) {
        int q = char_prev(l, p), i = q;
        int w = cwidth(utf8_get(l->buf, l->len, &i));

        if (c + w > cells) break;
        c += w;
        p = q;
    }
    return p;
}

static void line_view(struct line *l, int w, int hist)
{
    int v = w - hist;

    if (v < 1) v = 1;
    if (l->pos < l->first || tcols(l->buf + l->first, l->pos - l->first) >= v)
        l->first = back_cells(l, l->pos, w / 3);
}

/* The cells from the first character shown to the cursor. */
static int line_cells(const struct line *l)
{
    return l->pos > l->first ? tcols(l->buf + l->first, l->pos - l->first) : 0;
}

static void line_clear(struct line *l)
{
    l->len = l->pos = l->first = 0;
    l->buf[0] = 0;
}

static void line_cut(struct line *l, int a, int b)      /* delete [a, b) */
{
    int k = b - a;

    for (int i = a; i + k <= l->len; i++) l->buf[i] = l->buf[i + k];
    l->len -= k;
    l->pos = a;
    l->buf[l->len] = 0;
}

static void line_insert(struct line *l, int c)
{
    for (int i = l->len; i > l->pos; i--) l->buf[i] = l->buf[i - 1];
    l->buf[l->pos++] = (char)c;
    l->len++;
    l->buf[l->len] = 0;
}

/* Where the word before the cursor starts. */
static int word_left(const struct line *l)
{
    int p = l->pos;

    while (p > 0 && l->buf[p - 1] == ' ') p--;
    while (p > 0 && l->buf[p - 1] != ' ') p--;
    return p;
}

/* A character typed, as its UTF-8 bytes, at the cursor. */
static void line_char(struct line *l, uint32_t cp)
{
    char b[4];
    int n = utf8_enc(b, cp), j;

    if (l->len + n >= (int)sizeof l->buf) return;
    for (j = 0; j < n; j++) line_insert(l, (unsigned char)b[j]);
}

/* A key in a line being edited: cursor moves, deletion, typing. Keys that do
 * not edit are ignored. W and HIST give the field for the view. */
static void line_key(struct line *l, int key, int w, int hist)
{
    int p;

    switch (key) {
    case K_LEFT:   l->pos = char_prev(l, l->pos); break;
    case K_RIGHT:  l->pos = char_next(l, l->pos); break;
    case K_HOME:   l->pos = 0; break;
    case K_END:    l->pos = l->len; break;
    case K_CLEFT:  l->pos = word_left(l); break;
    case K_CRIGHT:
        p = l->pos;
        while (p < l->len && l->buf[p] == ' ') p++;
        while (p < l->len && l->buf[p] != ' ') p++;
        l->pos = p;
        break;
    case K_ALTBS:
        p = word_left(l);
        if (p < l->pos) line_cut(l, p, l->pos);
        break;
    case 8:
    case 127:
        if (l->pos > 0) line_cut(l, char_prev(l, l->pos), l->pos);
        break;
    case K_DEL:
        if (l->pos < l->len) line_cut(l, l->pos, char_next(l, l->pos));
        break;
    default:                       /* a byte of a UTF-8 character is typed as it comes */
        if (key & K_CHAR) {
            line_char(l, (uint32_t)(key & ~K_CHAR));
            break;
        }
        if (((key >= 0x20 && key < 0x7f) || (key >= 0x80 && key < 0x100)) &&
            l->len < (int)sizeof l->buf - 1)
            line_insert(l, key);
        break;
    }
    line_view(l, w, hist);
}

/* Draw the visible part of L, V columns wide, at X. */
static void line_draw(const struct line *l, int x, int v, uint16_t a)
{
    int shown = l->len > l->first ? cut_cols(l->buf + l->first, l->len - l->first, v) : 0;

    for (int i = 0; i < v; i++) setc(x + i, ' ', a);
    sets(x, l->buf + l->first, shown, a);
}

/* The input: the block read, from which the keys are taken, by the main loop and by
 * a question's wait alike. kbuf_eof is set when input has ended. */
static unsigned char kbuf[64];
static int kbuf_n, kbuf_i, kbuf_eof;

/* The last key taken was an Esc that another Esc followed at once. Each Esc after
 * such an Esc is taken at once too, as long as it comes within ESC_WAIT_MS of the one
 * before: it is a key of its own, not the start of a sequence (no sequence begins
 * with Esc Esc). esc_ts is the time of the last Esc of the chain. */
static int esc_chain;
static struct timespec esc_ts;      /* when the last Esc of the chain was taken */

/* The time of the last Esc of the chain: esc_ts. */
static void esc_mark(void)
{
    clock_gettime(CLOCK_REALTIME, &esc_ts);
}

/* Whether less than ESC_WAIT_MS has passed since esc_mark(). No 64-bit arithmetic, and
 * no division, which some targets would make a library call: the difference is taken
 * in seconds and nanoseconds, and only up to one second's difference is measured, so
 * it fits 32 bits. */
static int esc_recent(void)
{
    struct timespec ts;
    long ds, el;

    clock_gettime(CLOCK_REALTIME, &ts);
    ds = ts.tv_sec - esc_ts.tv_sec;
    if (ds < 0 || ds > 1) return 0;
    el = ds * 1000000000L + (ts.tv_nsec - esc_ts.tv_nsec);
    return el >= 0 && el < ESC_WAIT_MS * 1000000L;
}

/* The next key of the input block. A lone Esc at the end of the block may be the start
 * of a sequence, so it waits ESC_WAIT_MS for the rest, unless it follows an Esc taken
 * at once (above). The screen is drawn before such a wait, so what has been done shows
 * while the rest may come. When the block is used up it is read again. K_NONE: nothing
 * to handle here (a sequence cut off is dropped, or a read was interrupted). */
static int take_key(void)
{
    int len, key;

    for (;;) {
        if (kbuf_i >= kbuf_n) {
            long n = read(0, kbuf, sizeof kbuf);

            if (n == -EINTR) {
                check_size();
                return K_NONE;
            }
            if (n <= 0) {
                kbuf_eof = 1;
                return K_NONE;
            }
            kbuf_n = (int)n;
            kbuf_i = 0;
        }
        key = key_parse(kbuf, kbuf_n, kbuf_i, &len);
        if (key != K_INCOMPLETE) {
            kbuf_i += len;
            esc_chain = key == K_ESC;
            if (esc_chain) esc_mark();
            return key;
        }
        if (esc_chain && kbuf[kbuf_i] == K_ESC) {
            if (esc_recent()) {                      /* an Esc after an Esc: taken at once */
                kbuf_i++;
                esc_mark();
                return K_ESC;
            }
            esc_chain = 0;                           /* too long after the last: a new key */
        }
        draw();
        {
            int m = wait_input(kbuf, kbuf_n, sizeof kbuf, ESC_WAIT_MS);

            if (m > kbuf_n) {
                kbuf_n = m;
                continue;
            }
            esc_chain = 0;
            if (kbuf[kbuf_i] == K_ESC && kbuf_i + 1 == kbuf_n) {
                kbuf_i++;
                return K_ESC;
            }
            kbuf_i = kbuf_n;
            return K_NONE;
        }
    }
}

/* The keys of a question of the job: handled as usual, until the question is answered
 * (its handler leaves the question's mode). Called from inside the handler of the key
 * that started the job, reading from the block the main loop reads from. */
static void modal_wait(void)
{
    while (!kbuf_eof && (mode == MODE_QUERY || mode == MODE_CONFLICT)) {
        int key;

        draw();
        key = take_key();
        if (key != K_NONE) handle_key(key);
    }
}
