/* ---- zip */

#define ZIP_EOCD_SIG 0x06054b50u        /* end of central directory record */
#define ZIP_CEN_SIG  0x02014b50u        /* central directory file header */
#define ZIP_LOC_SIG  0x04034b50u        /* local file header */
#define ZIP_EOCD_LEN 22
#define ZIP_CEN_LEN  46

static uint32_t zip16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t zip32(const uint8_t *p) { return zip16(p) | zip16(p + 2) << 16; }

/* A zip's DOS date and time, which are local time, as seconds since 1970. */
static uint32_t zip_time(uint32_t dt, uint32_t dd)
{
    int64_t t = (int64_t)days_of(1980 + (int32_t)(dd >> 9), (int32_t)((dd >> 5) & 15),
                                 (int32_t)(dd & 31)) * 86400
              + (int64_t)(dt >> 11) * 3600 + (int64_t)((dt >> 5) & 63) * 60
              + (int64_t)(dt & 31) * 2;
    return (uint32_t)(t - tz_off(t));
}

static long zdata_put(void *arg, const uint8_t *p, size_t n)
{
    return write_all(*(long *)arg, p, n);
}

/* A zip member's data in the archive: stored, or deflated. */
struct zdata { const uint8_t *p; uint64_t n, usize; int method; };

static long zdata_fill(void *arg, long fd)
{
    const struct zdata *z = arg;
    uint64_t total = 0;
    long r;

    if (fd < 0) return fd;
    if (z->method == 0) return z->n == z->usize ? write_all(fd, z->p, z->n) : -EINVAL;
    r = inflate_raw(z->p, z->n, zdata_put, &fd, &total);
    if (r) return r;
    return total == z->usize ? 0 : -EINVAL;
}

/* A member of a zip archive, from its central directory record. */
struct zrec {
    const uint8_t *m;           /* the archive, mapped */
    uint64_t size;              /* its size */
    const uint8_t *name;        /* the name, not NUL-terminated */
    uint32_t nl, method, csize, usize, mtime, mode, off;
};

/* Calls CB for each record in the central directory of the zip file FD, SIZE bytes,
 * until CB returns non-zero. 0, or a negative errno: -EINVAL if it is not a zip that
 * can be read (zip64 is not), -EIO if it cannot be mapped. */
static long zip_walk(int fd, uint64_t size, long (*cb)(void *, const struct zrec *),
                     void *ctx)
{
    const uint8_t *m;
    uint64_t cdoff, cdend, pos;
    uint32_t count, k;
    int64_t i;

    if (size < ZIP_EOCD_LEN || size > (uint64_t)(size_t)-1) return -EINVAL;
    m = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) return -EIO;

    /* The end record: the last one of the file, or before a comment of up to 64 KiB,
     * whose comment length must reach exactly to the end of the file. */
    for (i = (int64_t)size - ZIP_EOCD_LEN;
         i >= 0 && i >= (int64_t)size - ZIP_EOCD_LEN - 65535; i--)
        if (zip32(m + i) == ZIP_EOCD_SIG &&
            (uint64_t)i + ZIP_EOCD_LEN + zip16(m + i + 20) == size) break;
    if (i < 0) {
        munmap((void *)m, (size_t)size);
        return -EINVAL;
    }
    count = zip16(m + i + 10);
    cdoff = zip32(m + i + 16);
    cdend = cdoff + zip32(m + i + 12);
    if (cdend > (uint64_t)i || count == 0xffff || cdoff == 0xffffffffu) {
        munmap((void *)m, (size_t)size);        /* zip64, or damaged */
        return -EINVAL;
    }

    for (k = 0, pos = cdoff; k < count && pos + ZIP_CEN_LEN <= cdend; k++) {
        const uint8_t *h = m + pos;
        struct zrec z;
        uint64_t len;

        if (zip32(h) != ZIP_CEN_SIG) break;
        z.nl = zip16(h + 28);
        len = ZIP_CEN_LEN + z.nl + zip16(h + 30) + zip16(h + 32);
        if (pos + len > cdend) break;
        z.m = m;
        z.size = size;
        z.name = h + ZIP_CEN_LEN;
        z.method = zip16(h + 10);
        z.csize = zip32(h + 20);
        z.usize = zip32(h + 24);
        z.mtime = zip_time(zip16(h + 12), zip16(h + 14));
        z.mode = zip32(h + 38) >> 16;
        z.off = zip32(h + 42);
        if (cb(ctx, &z)) break;
        pos += len;
    }
    munmap((void *)m, (size_t)size);
    return 0;
}

/* The index: each record is added. Stops when the index is full. */
static long zip_index_cb(void *ctx, const struct zrec *z)
{
    struct archive *a = ctx;
    char nb[256];

    if (z->nl > 255) return 0;
    for (uint32_t j = 0; j < z->nl; j++) nb[j] = (char)z->name[j];
    nb[z->nl] = 0;
    return archive_add(a, nb, z->usize, z->mtime, AE_FILE, NULL) < 0;
}

/* The extraction: a record taken is written, its data stored or inflated. */
static long zip_extract_cb(void *ctx, const struct zrec *z)
{
    struct xout *x = ctx;
    char nb[256];
    const char *name;
    size_t n;
    int dir;

    if (z->nl > 255) return 0;
    for (uint32_t j = 0; j < z->nl; j++) nb[j] = (char)z->name[j];
    nb[z->nl] = 0;
    name = norm_name(nb, &n, &dir);
    if (!name || !xwanted(x, name) || unsafe_name(name)) return 0;
    if (dir) {
        xdir(x, name);
        return 0;
    }
    if ((uint64_t)z->off + 30 > z->size || zip32(z->m + z->off) != ZIP_LOC_SIG) {
        xfail(x, -EINVAL);
        return 0;
    }
    if (z->method != 0 && z->method != 8) {
        xfail(x, -EINVAL);
        return 0;
    }
    {
        const uint8_t *h = z->m + z->off;
        uint64_t data = (uint64_t)z->off + 30 + zip16(h + 26) + zip16(h + 28);
        struct zdata zd;

        if (data > z->size || z->csize > z->size - data) {
            xfail(x, -EINVAL);
            return 0;
        }
        zd.p = z->m + data;
        zd.n = z->csize;
        zd.usize = z->usize;
        zd.method = (int)z->method;
        xmember(x, name, z->mode & 0777 ? (int)(z->mode & 0777) : 0644, zdata_fill, &zd);
    }
    return 0;
}
