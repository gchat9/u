/* fmt.h - sizes and dates, as the panels show them. */

/* A size, in 32 bits: a 30-bit significand and a 2-bit unit (0 bytes, 1 KiB,
 * 1 MiB, 1 GiB).  Exact below 1 GiB; above, to the unit, saturating at 1 EiB.
 * It is for display: a file operation must stat the file again. */
static uint32_t size_pack(uint64_t s)
{
    unsigned u = 0;

    while (u < 3 && s >= (1ull << (30 + 10 * u))) u++;
    if (u) s = (s + (1ull << (10 * u - 1))) >> (10 * u);
    if (s >= (1ull << 30)) s = (1ull << 30) - 1;
    return (uint32_t)((s << 2) | u);
}

static uint64_t size_unpack(uint32_t v)
{
    return (uint64_t)(v >> 2) << (10 * (v & 3));
}

static uint64_t ent_size(const struct ent *e) { return size_unpack(e->size); }

/* ------------------------------------------------------------- sizes */

/* Format SIZE in units from J upwards (0 = bytes, 1 = KiB, ...) the way mc's
 * size_trunc_len() does, into at most LEN characters. Returns the length. */
static int fmt_size(char *b, int len, uint64_t size, int j)
{
    static const uint64_t p10[] = { 1, 10, 100, 1000, 10000, 100000,
                                    1000000, 10000000, 100000000 };
    static const char *const sfx[] = { "", "K", "M", "G", "T", "P", "E" };
    int units = j, n = 0;

    for (; j < 7; j++) {
        if (size == 0) {
            if (j == units) { b[0] = '0'; return 1; }
            if (len > 1) b[n++] = '~';
            n += cat(b + n, j > 1 ? sfx[j - 1] : "B");
            return n;
        }
        if (size < p10[len - (j > 0)]) {
            n = (int)utoa((unsigned long)size, b);
            n += cat(b + n, sfx[j]);
            return n;
        }
        size = (size + 512) >> 10;
    }
    b[0] = '?';
    return 1;
}

/* ------------------------------------------------------------- times */

/* UTC offset in seconds at time T, from /etc/localtime (TZif). Uses the
 * 64-bit block when the file has one. POSIX TZ footers are not read, so DST
 * rules after the file's last explicit transition are not applied. */
static int64_t tz_off(int64_t t)
{
    const uint8_t *h = tzb;
    uint64_t tc, ty, k = 0, i;
    int w = 4;

    if (tzn < 44 || h[0] != 'T' || h[1] != 'Z' || h[2] != 'i' || h[3] != 'f')
        return 0;
    if (h[4] >= '2') {
        /* skip the 32-bit block: times, indices, ttinfo, abbrevs, leaps, flags */
        uint64_t v1 = be(h + 32, 4) * 5 + be(h + 36, 4) * 6 + be(h + 40, 4) +
                      be(h + 28, 4) * 8 + be(h + 24, 4) + be(h + 20, 4);
        if ((uint64_t)tzn < v1 + 88) return 0;
        h += 44 + v1;
        w = 8;
    }
    tc = be(h + 32, 4);
    ty = be(h + 36, 4);
    if ((uint64_t)(h - tzb) + 44 + tc * (w + 1) + ty * 6 > (uint64_t)tzn) return 0;

    const uint8_t *tt = h + 44, *ix = tt + tc * w, *ti = ix + tc;
    for (i = 0; i < tc; i++) {
        int64_t when = w == 8 ? (int64_t)be(tt + i * 8, 8)
                              : (int64_t)(int32_t)(uint32_t)be(tt + i * 4, 4);
        if (when > t) break;
        k = ix[i];
    }
    if (k >= ty) return 0;
    return (int32_t)(uint32_t)be(ti + k * 6, 4);
}

/* Days since 1970-01-01 to a civil date (proleptic Gregorian). 32-bit is
 * enough for any date a file can have, and is cheaper on 32-bit targets. */
static void civil(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;

    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)((int32_t)yoe + era * 400 + (*m <= 2));
}

/* mc's date column, 12 characters: "Nov  1 16:48" when recent,
 * "Nov  1  2025" when older than six months or in the future. */
static void fmt_date(char *b, int64_t mt)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int64_t t = mt + tz_off(mt);
    uint64_t mag = t < 0 ? (uint64_t)-t : (uint64_t)t, rem, q = udiv64(mag, 86400, &rem);
    int64_t days;
    int sod, y, m, d;

    if (t < 0) {                 /* floor division for times before 1970 */
        if (rem) { q++; rem = 86400 - rem; }
        days = -(int64_t)q;
    } else {
        days = (int64_t)q;
    }
    sod = (int)rem;
    civil((int32_t)days, &y, &m, &d);

    b[0] = mon[(m - 1) * 3];
    b[1] = mon[(m - 1) * 3 + 1];
    b[2] = mon[(m - 1) * 3 + 2];
    b[3] = ' ';
    putnum(b + 4, (unsigned long)d, 2);
    if (now > mt + 6L * 30 * 24 * 3600 || now < mt - 3600) {
        b[6] = b[7] = ' ';
        putnum(b + 8, (unsigned long)y, 4);
    } else {
        b[6] = ' ';
        b[7] = (char)('0' + sod / 3600 / 10);
        b[8] = (char)('0' + sod / 3600 % 10);
        b[9] = ':';
        b[10] = (char)('0' + sod % 3600 / 600);
        b[11] = (char)('0' + sod % 3600 / 60 % 10);
    }
}

/* Days since 1970-01-01 of a date: the inverse of civil(). */
static int32_t days_of(int32_t y, int32_t m, int32_t d)
{
    int32_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
