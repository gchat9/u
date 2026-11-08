/* ---- inflate: the raw deflate stream (RFC 1951) of a zip member, written out in
 * pieces. The last INF_WIN bytes written are kept, as a back reference may reach
 * that far. */
#define INF_WIN 32768u
#define INF_OUT 65536u

struct huff { uint16_t count[16]; uint16_t sym[288]; };

struct infl {
    const uint8_t *in;
    size_t n, i;                /* the input, and the next byte of it */
    uint32_t bb;                /* the byte being read: its bits not used yet */
    int nb;                     /* how many of them are left */
    uint8_t out[INF_OUT];
    size_t pos;                 /* bytes of out in use */
    uint64_t total;             /* bytes written so far, in all */
    long err;                   /* a negative errno, once the stream is damaged */
    long (*put)(void *, const uint8_t *, size_t);
    void *arg;
};

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2,
    14, 1, 15 };

static void inf_put(struct infl *s, const uint8_t *p, size_t n)
{
    if (!s->err && n) {
        long r = s->put(s->arg, p, n);

        if (r < 0) s->err = r;
    }
}

/* The next NEED bits of the input, the first one least significant. */
static uint32_t inf_bits(struct infl *s, int need)
{
    uint32_t v = 0;

    for (int k = 0; k < need; k++) {
        if (!s->nb) {
            if (s->i >= s->n) {
                s->err = -EIO;
                return 0;
            }
            s->bb = s->in[s->i++];
            s->nb = 8;
        }
        v |= (s->bb & 1) << k;
        s->bb >>= 1;
        s->nb--;
    }
    return v;
}

/* Writes out all of the window but its last INF_WIN bytes, which stay. */
static void inf_slide(struct infl *s)
{
    size_t keep = s->pos - INF_WIN, k;

    inf_put(s, s->out, keep);
    for (k = 0; k < INF_WIN; k++) s->out[k] = s->out[keep + k];
    s->pos = INF_WIN;
}

static void inf_byte(struct infl *s, uint8_t c)
{
    if (s->pos == INF_OUT) inf_slide(s);
    s->out[s->pos++] = c;
    s->total++;
}

/* The canonical Huffman code of the N lengths in LEN, for decoding. */
static void huff_build(struct huff *h, const uint8_t *len, int n)
{
    uint16_t offs[16];

    for (int l = 0; l < 16; l++) h->count[l] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

/* One symbol of the code H, read bit by bit. */
static int huff_decode(struct infl *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        int count;

        code |= (int)inf_bits(s, 1);
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = -EINVAL;
    return 0;
}

static void inf_codes(struct infl *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym, len, d;
        uint32_t dist;

        sym = huff_decode(s, lc);
        if (s->err) return;
        if (sym < 256) {
            inf_byte(s, (uint8_t)sym);
            continue;
        }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) {
            s->err = -EINVAL;
            return;
        }
        len = lbase[sym] + (int)inf_bits(s, lext[sym]);
        d = huff_decode(s, dc);
        if (s->err) return;
        if (d >= 30) {
            s->err = -EINVAL;
            return;
        }
        dist = dbase[d] + inf_bits(s, dext[d]);
        if (s->err) return;
        if (dist > s->total) {
            s->err = -EINVAL;
            return;
        }
        while (len--) inf_byte(s, s->out[s->pos - dist]);
    }
}

/* A block stored as it is: its length is checked against its complement. */
static void inf_stored(struct infl *s)
{
    uint32_t len, nlen;

    s->nb = 0;                          /* the rest of the byte is not used */
    if (s->i + 4 > s->n) {
        s->err = -EIO;
        return;
    }
    len = s->in[s->i] | s->in[s->i + 1] << 8;
    nlen = s->in[s->i + 2] | s->in[s->i + 3] << 8;
    s->i += 4;
    if (len != (~nlen & 0xffffu)) {
        s->err = -EINVAL;
        return;
    }
    if (len > s->n - s->i) {
        s->err = -EIO;
        return;
    }
    while (len--) inf_byte(s, s->in[s->i++]);
}

/* A block with the fixed codes of the format. */
static void inf_fixed(struct infl *s)
{
    uint8_t len[288 + 30];
    struct huff lc, dc;
    int i;

    for (i = 0; i < 144; i++) len[i] = 8;
    for (; i < 256; i++) len[i] = 9;
    for (; i < 280; i++) len[i] = 7;
    for (; i < 288; i++) len[i] = 8;
    huff_build(&lc, len, 288);
    for (i = 0; i < 30; i++) len[i] = 5;
    huff_build(&dc, len, 30);
    inf_codes(s, &lc, &dc);
}

/* A block with codes of its own, sent first. */
static void inf_dynamic(struct infl *s)
{
    uint8_t len[320];
    struct huff cl, lc, dc;
    int nlen, ndist, ncode, index = 0;

    nlen = (int)inf_bits(s, 5) + 257;
    ndist = (int)inf_bits(s, 5) + 1;
    ncode = (int)inf_bits(s, 4) + 4;
    if (s->err) return;
    if (nlen > 286 || ndist > 30) {
        s->err = -EINVAL;
        return;
    }
    for (int i = 0; i < 19; i++) len[i] = 0;
    for (int i = 0; i < ncode; i++) len[clorder[i]] = (uint8_t)inf_bits(s, 3);
    huff_build(&cl, len, 19);
    while (index < nlen + ndist) {
        int sym, rep, val = 0;

        sym = huff_decode(s, &cl);
        if (s->err) return;
        if (sym < 16) {
            len[index++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!index) {
                s->err = -EINVAL;
                return;
            }
            val = len[index - 1];
            rep = 3 + (int)inf_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)inf_bits(s, 3);
        } else {
            rep = 11 + (int)inf_bits(s, 7);
        }
        if (index + rep > nlen + ndist) {
            s->err = -EINVAL;
            return;
        }
        while (rep--) len[index++] = (uint8_t)val;
    }
    if (len[256] == 0) {
        s->err = -EINVAL;
        return;
    }
    huff_build(&lc, len, nlen);
    huff_build(&dc, len + nlen, ndist);
    inf_codes(s, &lc, &dc);
}

/* Inflates the raw deflate stream IN, N bytes, writing its output through PUT (which
 * returns 0 or a negative errno). *TOTAL, if given, is the output's size. 0, or a
 * negative errno. */
static long inflate_raw(const uint8_t *in, size_t n,
                        long (*put)(void *, const uint8_t *, size_t), void *arg,
                        uint64_t *total)
{
    static struct infl s;
    int last;

    s.in = in;
    s.n = n;
    s.i = 0;
    s.bb = 0;
    s.nb = 0;
    s.pos = 0;
    s.total = 0;
    s.err = 0;
    s.put = put;
    s.arg = arg;
    do {
        uint32_t type;

        last = (int)inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (s.err) break;
        if (type == 0) inf_stored(&s);
        else if (type == 1) inf_fixed(&s);
        else if (type == 2) inf_dynamic(&s);
        else s.err = -EINVAL;
    } while (!last && !s.err);
    inf_put(&s, s.out, s.pos);
    if (total) *total = s.total;
    return s.err;
}
