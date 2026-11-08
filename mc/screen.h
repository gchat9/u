/* screen.h - the terminal: the output buffer, the cells of a frame and how a row
 * of them is written, the terminal modes. */

/* ---------------------------------------------------------------- output */

static void flush(void)
{
    int off = 0;

    while (off < on) {
        long n = write(1, ob + off, (size_t)(on - off));

        if (n == -EINTR) continue;           /* a window change interrupted us */
        if (n <= 0) break;
        off += (int)n;
    }
    on = 0;
}

static void put(const char *s, int n)
{
    while (n--) {
        if (on == (int)sizeof ob) flush();
        ob[on++] = *s++;
    }
}

static void outs(const char *s) { put(s, (int)strlen(s)); }

static void outu(unsigned long v)
{
    char b[24];
    put(b, (int)utoa(v, b));
}

static void setc(int x, uint32_t c, uint16_t a)
{
    if (x < 0 || x >= cols) return;
    /* a double-width character whose half is written over is broken: the other
     * half is left blank */
    if (x > 0 && row[x].c == CELL_TAIL) row[x - 1].c = ' ';
    if (x + 1 < cols && row[x + 1].c == CELL_TAIL) row[x + 1].c = ' ';
    row[x].c = c;
    row[x].a = a;
}

static void sets(int x, const char *s, int n, uint16_t a)
{
    for (int i = 0; i < n; ) {
        uint32_t cp = utf8_get(s, n, &i);
        int w = cwidth(cp);

        if (w == 0) continue;
        if (w == 2 && x + 1 >= cols) {          /* a double-width one that does not fit */
            setc(x, ' ', a);
            return;
        }
        setc(x, cp, a);
        if (w == 2) setc(x + 1, CELL_TAIL, a);
        x += w;
    }
}

/* S, N bytes of UTF-8, in W cells: all of it if it fits, else as much of its
 * end as fits after a ~ for what is cut from the front. */
static void show_tail(int x, const char *s, int n, int w, uint16_t a)
{
    int k = 0, total = tcols(s, n);

    if (total <= w) {
        sets(x, s, n, a);
        return;
    }
    while (total > w - 1 && k < n) total -= cwidth(utf8_get(s, n, &k));
    setc(x, '~', a);
    sets(x + 1, s + k, n - k, a);
}

/* DEC special graphics, 0x60..0x7e, as Unicode: what the line-drawing cells
 * stand for when the terminal is in a UTF-8 locale. */
static const uint16_t dec_uni[31] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
    0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd,
    0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3,
    0x00b7
};

static uint32_t dec_char(uint32_t c)
{
    return c >= 0x60 && c <= 0x7e ? dec_uni[c - 0x60] : c;
}

static void put_utf8(uint32_t cp)
{
    char b[4];

    put(b, utf8_enc(b, cp));
}

/* Does S name a UTF-8 encoding (UTF-8, utf8, utf-8 ...)? */
static int has_utf8(const char *s)
{
    for (; *s; s++) {
        if ((s[0] | 0x20) == 'u' && (s[1] | 0x20) == 't' && (s[2] | 0x20) == 'f') {
            const char *q = s + 3;

            if (*q == '-') q++;
            if (*q == '8') return 1;
        }
    }
    return 0;
}

/* Put row[0..n) on terminal line Y, emitting SGR and charset changes only
 * where the attributes change. */
static void emit_row(int y, int x0, int n)
{
    uint16_t cur = 0xFFFF;
    int dec = 0;

    outs("\x1b[");
    outu((unsigned long)y + 1);
    outs(";");
    outu((unsigned long)x0 + 1);
    outs("H");
    for (int i = x0; i < n; i++) {
        uint16_t a = row[i].a;
        uint16_t key = a & ~DEC;

        if (row[i].c == CELL_TAIL) continue;    /* the right half of a wide character */
        if (key != cur) {
            cur = key;
            if (a & DEFA) {
                outs("\x1b[0m");
            } else {
                outs("\x1b[0;");
                if (a & 0x40) outs("1;");
                outs((a & BRIGHT) ? "9" : "3");
                outu(a & 7);
                outs(";4");
                outu((a >> 3) & 7);
                outs("m");
            }
        }
        if (((a & DEC) != 0 && !utf8_lines) != dec) {
            dec = !dec;
            outs(dec ? "\x1b(0" : "\x1b(B");
        }
        if ((a & DEC) && utf8_lines) put_utf8(dec_char(row[i].c));
        else if (row[i].c < 0x80) {
            char b = (char)row[i].c;

            put(&b, 1);
        } else put_utf8(row[i].c);
    }
    if (dec) outs("\x1b(B");
    outs("\x1b[0m");
}

/* Shadow cells x0..x1 of row Y: the character stays, the colour goes dark. */
static void shade(int x0, int x1)
{
    for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < cols; x++)
        row[x].a = C_SHADE | (row[x].a & DEC);   /* a line-drawing cell stays one */
}

static void tty_leave(void)
{
    rfull = 1;
    if (tty_ok) ioctl(0, TCSETS, &tty_saved);
    outs("\x1b[0m");
    if (on_alt) {
        outs("\x1b[?1049l");
        on_alt = 0;
    }
    flush();
}

static void tty_enter(void)
{
    rfull = 1;
    if (tty_ok) ioctl(0, TCSETS, &tty_raw);
    if (!hidden && !on_alt) {
        outs("\x1b[?1049h");
        on_alt = 1;
    }
    flush();
}

/* Give the pseudo-terminal FD the size of the terminal. */
static void size_pty(int fd)
{
    struct winsz ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };

    ioctl(fd, TIOCSWINSZ, &ws);
}
