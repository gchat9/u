#define TAR_BLK 512

/* A tar entry's data: the next SIZE bytes of the stream IN. */
struct tdata { long in; uint64_t size; };

static long tdata_fill(void *arg, long fd)
{
    const struct tdata *t = arg;
    uint8_t buf[4096];
    uint64_t left = t->size;
    long r = fd < 0 ? fd : 0;

    while (left) {
        size_t k = left < sizeof buf ? (size_t)left : sizeof buf;

        if (read_full(t->in, buf, k) != k) return -EIO;
        if (!r) r = write_all(fd, buf, k);
        left -= k;
    }
    return r;
}

/* A number of N bytes in a tar header: octal, or the GNU form (base-256) for big
 * values. *bad is set for anything else. */
static uint64_t tar_num(const uint8_t *p, int n, int *bad)
{
    uint64_t v = 0;
    int i = 0;

    if (p[0] & 0x80) {
        v = p[0] & 0x7f;
        for (i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    while (i < n && p[i] >= '0' && p[i] <= '7') v = v << 3 | (uint64_t)(p[i++] - '0');
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    if (i < n) *bad = 1;
    return v;
}

/* A header of zeros is the end of the archive. */
static int tar_zero(const uint8_t *h)
{
    for (int i = 0; i < TAR_BLK; i++)
        if (h[i]) return 0;
    return 1;
}

/* The checksum of a header: its bytes added up, the checksum field counting as spaces. */
static int tar_sum_ok(const uint8_t *h)
{
    uint32_t sum = 0;
    int bad = 0;

    for (int i = 0; i < TAR_BLK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return tar_num(h + 148, 8, &bad) == sum && !bad;
}

/* Skip N bytes of the stream FD: a seek where FD is a file, else read and drop them. -1
 * if it cannot be done, or N is more than a terabyte (it would be a million seeks, or
 * a read of that much, to find out that the file ends). */
static int tar_skip(long fd, uint64_t n)
{
    char buf[4096];

    if (n >> 40) return -1;                 /* not a size of any file: a damaged header */
    if (lseek((int)fd, 0, SEEK_CUR) >= 0) {
        while (n) {
            uint64_t k = n < (1u << 30) ? n : (1u << 30);

            if (lseek((int)fd, (long)k, SEEK_CUR) < 0) return -1;
            n -= k;
        }
        return 0;
    }
    while (n) {
        size_t k = n < sizeof buf ? (size_t)n : sizeof buf;

        if (read_full(fd, buf, k) != k) return -1;
        n -= k;
    }
    return 0;
}

/* What the headers before an entry say about it: a long name or link (GNU L and K,
 * or pax path and linkpath), a size and a date (pax). The flags are 1 when set, and 2
 * when the text is too long to keep: then the entry is not listed. */
struct tar_meta {
    char     gname[256];                /* GNU L: the name of the next entry */
    char     pname[256];                /* pax path */
    char     lname[256];                /* GNU K, or pax linkpath: a link's target */
    int      hg, hp, hk;
    uint64_t psize, pmtime;
    int      hs, hm;
};

/* The text of N bytes at B, up to a NUL, copied to OUT (*flag is 1), or too long (2). */
static void tar_text(const uint8_t *b, uint64_t n, char *out, int *flag)
{
    uint64_t k = 0;

    while (k < n && b[k]) k++;
    if (k > 255) {
        *flag = 2;
        return;
    }
    for (uint64_t j = 0; j < k; j++) out[j] = (char)b[j];
    out[k] = 0;
    *flag = 1;
}

/* Whether the N bytes at S are the keyword KW. */
static int tar_kw(const uint8_t *s, uint64_t n, const char *kw)
{
    for (uint64_t k = 0; k < n; k++)
        if (!kw[k] || kw[k] != (char)s[k]) return 0;
    return kw[n] == 0;
}

/* A decimal number at the start of N bytes at B, to *out. 0 if there is none. */
static int tar_dec(const uint8_t *b, uint64_t n, uint64_t *out)
{
    uint64_t v = 0, k = 0;

    if (!n || b[0] < '0' || b[0] > '9') return 0;
    while (k < n && b[k] >= '0' && b[k] <= '9') v = v * 10 + (uint64_t)(b[k++] - '0');
    *out = v;
    return 1;
}

/* The records of a pax extended header, B, N bytes: "LEN KEY=VALUE" and a newline,
 * one after another. The keys path, linkpath, size and mtime are taken. */
static void tar_pax(const uint8_t *b, uint64_t n, struct tar_meta *m)
{
    uint64_t i = 0;

    while (i < n) {
        uint64_t len = 0, k = 0, kb, e, vs, ve;

        while (i + k < n && b[i + k] >= '0' && b[i + k] <= '9')
            len = len * 10 + (uint64_t)(b[i + k++] - '0');
        if (i + k >= n || k == 0 || len == 0 || len > n - i || b[i + k] != ' ') return;

        kb = i + k + 1;                     /* the key starts after the space */
        e = kb;
        while (e < i + len && b[e] != '=') e++;
        if (e >= i + len) return;
        vs = e + 1;
        ve = i + len - 1;                   /* the value ends before the newline */
        if (ve < vs) ve = vs;

        if (tar_kw(b + kb, e - kb, "path")) {
            tar_text(b + vs, ve - vs, m->pname, &m->hp);
        } else if (tar_kw(b + kb, e - kb, "linkpath")) {
            tar_text(b + vs, ve - vs, m->lname, &m->hk);
        } else if (tar_kw(b + kb, e - kb, "size")) {
            m->hs = tar_dec(b + vs, ve - vs, &m->psize);
        } else if (tar_kw(b + kb, e - kb, "mtime")) {
            m->hm = tar_dec(b + vs, ve - vs, &m->pmtime);
        }
        i += len;
    }
}

/* The name of the ustar header H: its prefix, a "/", and its name, into OUT. 0 if it
 * is too long to keep. The prefix is only there in the POSIX form ("ustar" and NUL),
 * which the GNU form does not share. */
static int tar_ustar(const uint8_t *h, char *out)
{
    static const char magic[6] = { 'u', 's', 't', 'a', 'r', 0 };
    size_t k = 0, i;
    int posix = 1;

    for (i = 0; i < 6; i++)
        if ((char)h[257 + i] != magic[i]) posix = 0;
    if (posix) {
        for (i = 0; i < 155 && h[345 + i]; i++)
            ;
        if (i) {
            if (i + 1 > 255) return 0;
            for (k = 0; k < i; k++) out[k] = (char)h[345 + k];
            out[k++] = '/';
        }
    }
    for (i = 0; i < 100 && h[i]; i++)
        ;
    if (k + i > 255) return 0;
    for (size_t j = 0; j < i; j++) out[k + j] = (char)h[j];
    out[k + i] = 0;
    return 1;
}

/* An entry of a tar stream, taken: a file's data is read from FD and written, the rest
 * is written as it is. Returns the bytes of data read; 0 for an entry not taken, whose
 * data the caller passes over. */
static uint64_t xtar_entry(struct xout *x, long fd, const char *name, int kind, int mode,
                           uint64_t size, const char *target)
{
    size_t n;
    int dir;
    const char *nn = norm_name(name, &n, &dir);
    struct tdata td;

    if (!nn || !xwanted(x, nn) || unsafe_name(nn)) return 0;
    if (dir || kind == AE_DIR) {
        xdir(x, nn);
        return 0;
    }
    if (kind == AE_LINK) {
        xlink(x, nn, target);
        return 0;
    }
    if (kind == AE_HARD) {
        xhard(x, nn, target);
        return 0;
    }
    td.in = fd;
    td.size = size;
    xmember(x, nn, mode ? mode : 0644, tdata_fill, &td);
    return size;
}

/* Reads the tar stream FD, header after header, and passes over the data of each
 * entry. With A, each entry is indexed; with X, the entries taken are extracted. PID
 * is the process writing the stream, or 0 if FD is the file itself. If the index
 * fills, *full is set and the rest is not read. 0, or -EINVAL if the stream is not
 * tar. */
static int tar_scan(struct archive *a, long fd, long pid, int *full, struct xout *x)
{
    uint8_t h[TAR_BLK];
    char lbuf[4096], nm[256], tg[256];
    struct tar_meta m;
    int bad = 0, eoa = 0, flag;

    *full = 0;
    zero_block(&m, sizeof m);
    for (;;) {
        uint64_t size, mtime, eff, pad, blk, used = 0;
        int t, kind, mode, r = 0;

        if (read_full(fd, h, TAR_BLK) != TAR_BLK) break;
        if (tar_zero(h)) {
            eoa = 1;
            break;
        }
        if (!tar_sum_ok(h)) {
            bad = 1;
            break;
        }
        size = tar_num(h + 124, 12, &bad);
        mtime = tar_num(h + 136, 12, &bad);
        if (bad) break;
        t = h[156];
        blk = (size + TAR_BLK - 1) & ~(uint64_t)(TAR_BLK - 1);

        if (t == 'x' || t == 'g' || t == 'L' || t == 'K') {
            if (size <= sizeof lbuf) {
                if (read_full(fd, lbuf, size) != size) break;
                if (tar_skip(fd, blk - size)) break;
                if (t == 'x') tar_pax((uint8_t *)lbuf, size, &m);
                else if (t == 'L') tar_text((uint8_t *)lbuf, size, m.gname, &m.hg);
                else if (t == 'K') tar_text((uint8_t *)lbuf, size, m.lname, &m.hk);
            } else {
                if (tar_skip(fd, blk)) break;
                if (t == 'x') m.hp = 2;     /* too big to read: its path is unknown */
                else if (t == 'L') m.hg = 2;
                else if (t == 'K') m.hk = 2;
            }
            continue;
        }

        eff = m.hs ? m.psize : size;
        pad = (eff + TAR_BLK - 1) & ~(uint64_t)(TAR_BLK - 1);
        kind = t == '5' ? AE_DIR : t == '2' ? AE_LINK : t == '1' ? AE_HARD : AE_FILE;
        mode = (int)(tar_num(h + 100, 8, &flag) & 0777);
        if (m.hg != 2 && m.hp != 2 && !(kind == AE_LINK && m.hk == 2)) {
            const char *name = nm, *target = NULL;
            int ok = 1;

            if (m.hp) name = m.pname;
            else if (m.hg) name = m.gname;
            else ok = tar_ustar(h, nm);
            if (kind == AE_LINK || kind == AE_HARD) {
                if (m.hk) {
                    target = m.lname;
                } else {
                    tar_text(h + 157, 100, tg, &flag);
                    target = tg;
                }
            }
            if (ok) {
                if (x) used = xtar_entry(x, fd, name, kind, mode, eff, target);
                else r = archive_add(a, name, eff, m.hm ? m.pmtime : mtime, kind, target);
            }
        }
        if (r < 0) {
            *full = 1;
            break;
        }
        if (tar_skip(fd, pad - used)) break;
        zero_block(&m, sizeof m);
    }
    if (pid && !*full)
        while (read_full(fd, lbuf, sizeof lbuf) == sizeof lbuf)
            ;                               /* the rest, so that the process ends cleanly */
    if (a && bad && a->n == 0 && !eoa) return -EINVAL;
    return 0;
}
