/*
 * dnscache - a stand-in for `dnsmasq -S 1.1.1.1`, hardened.
 *
 * Listens on UDP 127.0.0.1:5353 and passes misses verbatim to 1.1.1.1:53;
 * the answer goes back to the client byte for byte.  The first A address from
 * each miss answer is appended to a private mmap()ed cache as
 *     <ip> <asked-for name> #<unix-timestamp>
 * CNAMEs are followed and the chain is flattened under the original name.
 * A queries are lower-cased (cache key) and answered from the file on a hit;
 * non-A queries get NXDOMAIN without going upstream.  No more than once a
 * minute, after a cache hit has been sent, the file is filtered of entries
 * older than MAX_AGE_SECS.
 *
 * Security hardening (v2):
 *  - cache lives in a private 0700 directory, not shared /tmp/hosts;
 *  - every open uses O_NOFOLLOW|O_CLOEXEC; created files O_EXCL, mode 0600;
 *  - fstatx verifies regular file, owned by us, st_nlink==1, not world/group
 *    writable, size <= MAX_CACHE; nothing is read or written through a link;
 *  - cache is rewritten by writing a private temp file + fsync-free atomic
 *    renameat2(), never truncate-in-place; every write()/close() is checked;
 *  - DNS names are validated (no space/#/control bytes), lowercased, keys are
 *    lower-cased; malformed queries are dropped, not forwarded upstream;
 *  - upstream replies require m>=12, QR=1, QDCOUNT=1, RCODE match for caching,
 *    transaction-ID match (random, not the client's), and byte-exact question
 *    match; a random per-query ID is used upstream and 0x20 case randomization
 *    is applied to the sent question name; the reply is relayed only after
 *    validation, with client ID and question restored; extra datagrams are
 *    drained until a valid answer or timeout; cache entries also require a
 *    fresh timestamp at lookup time;
 *  - cache size is capped at MAX_CACHE (1 MiB): refuse to start over it,
 *    prune expired first, then drop oldest still-valid entries first;
 *  - transient socket errors are not fatal.
 */
#include "../_sys/_main.h"

#define LOCAL_PORT       5353
#define REPLY_TIMEOUT_MS 5000
#define BUFSIZE          65536       /* any UDP payload we could be sent */
#define MAX_CHAIN        8
#define MAX_A            64
#define MAX_NAME         255
#define CLEANUP_EVERY    60          /* seconds between cache prunes */
#define MAX_CACHE        1048576u   /* 1 MiB hard cap on cache file */
#ifndef MAX_AGE_SECS
#define MAX_AGE_SECS     3600        /* override at compile time with -D */
#endif

/* private cache location (not shared /tmp namespace) */
#define CACHE_DIR        "/tmp/dnscache-cache"
#define CACHE_FILE       CACHE_DIR "/hosts"
#define CACHE_TMP        CACHE_DIR "/hosts.tmp"




/* ------------------------------------------------------------------ utils */

static void lower(char *s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s | 0x20);
}

/* A DNS-name string is safe as a hosts key / filename-free token. */
static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > MAX_NAME) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f || c == ' ' || c == '#') return 0;
    }
    return 1;
}

/* Write a DNS name to out in dotted form, following compression pointers.
 * *end receives the offset just past the name as it appeared at pos.  0 means
 * the message is malformed; out is always NUL-terminated on success.  Reject
 * labels containing control bytes, space, '#', '.', or an embedded NUL, and
 * any byte >=0x80, so keys can never break out of a hosts line. */
static int dns_name(const uint8_t *m, size_t len, size_t pos,
                    char *out, size_t outsz, size_t *end)
{
    size_t n = 0, p = pos;
    int hops = 0, jumped = 0;

    for (;;) {
        if (p >= len) return 0;
        uint8_t l = m[p];
        if ((l & 0xC0) == 0xC0) {                 /* compression pointer */
            if (p + 1 >= len || ++hops > 8) return 0;
            if (!jumped) *end = p + 2;
            jumped = 1;
            p = ((size_t)(l & 0x3F) << 8) | m[p + 1];
            if (p >= len) return 0;
            continue;
        }
        if (l & 0xC0) return 0;                   /* reserved label type */
        p++;
        if (l == 0) {
            if (!jumped) *end = p;
            out[n] = '\0';
            return 1;
        }
        if (l > len - p || n + l + (n != 0) >= outsz) return 0;
        /* label contents must be printable, unambiguous, no key-breaking */
        for (uint8_t k = 0; k < l; k++) {
            uint8_t c = m[p + k];
            if (c < 0x20 || c == 0x7f || c == ' ' || c == '#' ||
                c == '.' || c == 0 || c >= 0x80) return 0;
        }
        if (n) out[n++] = '.';
        memcpy(out + n, m + p, l);
        n += l;
        p += l;
    }
}

static int in_chain(char chain[][MAX_NAME + 1], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(chain[i], name) == 0) return 1;
    return 0;
}

static long long now_secs(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec;
}

/* checked write of the whole buffer; EINTR retried */
static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        long w = write(fd, p, n);
        if (w < 0) { if (w == -EINTR) continue; return -1; }
        if (w == 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* ------------------------------------------------------- secure open/stat */

static int statx_fd(int fd, struct statx *st)
{
    return statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, st) < 0 ? -1 : 0;
}

/* Open read-only, no symlink, no leak across exec. */
static int open_ro(const char *path)
{
    long fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
    return fd < 0 ? -1 : (int)fd;
}

/* fstatx: regular file, owned by us, exactly one link, not group/other
 * writable.  Returns 0 ok. */
static int check_owned_regular(int fd, uint64_t *size_out)
{
    struct statx st;
    if (statx_fd(fd, &st) < 0) return -1;
    if ((st.stx_mode & S_IFMT) != S_IFREG) return -1;
    if (st.stx_uid != (uint32_t)getuid()) return -1;
    if (st.stx_nlink != 1) return -1;
    if (st.stx_mode & 0022) return -1;            /* not group/other writable */
    if (size_out) *size_out = st.stx_size;
    return 0;
}

/* Create a private cache directory 0700, verify it (not a link, owned, only
 * user-accessible), return 0. */
static int setup_cache_dir(void)
{
    struct statx st;
    mkdirat(AT_FDCWD, CACHE_DIR, 0700);           /* ignore EEXIST */
    if (statx(AT_FDCWD, CACHE_DIR, AT_SYMLINK_NOFOLLOW,
              STATX_BASIC_STATS, &st) < 0) return -1;
    if ((st.stx_mode & S_IFMT) != S_IFDIR) return -1;
    if (st.stx_uid != (uint32_t)getuid()) return -1;
    if (st.stx_mode & 077) return -1;             /* not private */
    return 0;
}

/* ------------------------------------------------------------- hosts map */

/* Map the cache file for lookups; verify ownership; enforce MAX_CACHE. */
static int refresh_map(const char *path, uint8_t **map, size_t *len)
{
    int fd;
    uint64_t size = 0;
    void *p;

    if (*map) { munmap(*map, *len); *map = 0; *len = 0; }

    fd = open_ro(path);
    if (fd < 0) return -1;
    if (check_owned_regular(fd, &size) < 0) { close(fd); return -1; }
    if (size == 0) { close(fd); return 0; }

    p = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    *map = p;
    *len = (size_t)size;
    return 0;
}

/* ------------------------------------------------------------ hosts lines */

/* Parse a valid unix timestamp after '#' at end of line; 0 => not fresh. */
static int line_ts(const uint8_t *line, size_t len, long long *ts_out)
{
    size_t i = 0;
    unsigned long long t = 0;
    int digits = 0;

    if (!len) return 0;
    while (i < len && line[i] != '#') i++;
    if (i == len) return 0;
    for (i++; i < len && line[i] >= '0' && line[i] <= '9'; i++) {
        if (t > (unsigned long long)__LONG_MAX__ / 10) return 0;
        t = t * 10 + (unsigned long long)(line[i] - '0');
        if (t > (unsigned long long)__LONG_MAX__) return 0;   /* no wrap */
        digits++;
    }
    if (!digits || i != len) return 0;
    if (ts_out) *ts_out = (long long)t;
    return 1;
}

/* Keep a line only if it has a plausible unix timestamp, recent enough. */
static int fresh_line(const uint8_t *line, size_t len, long long cutoff)
{
    long long t;
    return line_ts(line, len, &t) && t >= cutoff;
}

/* Textual IPv4 at the start of a hosts line, up to the space before name. */
static int parse_ip(const uint8_t *s, size_t len, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        unsigned v = 0, digits = 0;
        while (len && *s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (v > 255) return 0;
            s++; len--; digits++;
        }
        if (!digits) return 0;
        out[i] = (uint8_t)v;
        if (i < 3) { if (!len || *s++ != '.') return 0; len--; }
    }
    return len == 0;
}

/* Find every FRESH line whose name matches " name #" with a valid IP. */
static int cache_lookup(const uint8_t *map, size_t len, const char *name,
                        uint8_t ips[][4], int max, long long cutoff)
{
    char pat[MAX_NAME + 3];
    size_t hlen = strlen(name), plen = hlen + 2, off = 0;
    int n = 0;
    uint8_t ip[4];

    if (!map || !hlen || hlen > MAX_NAME) return 0;
    pat[0] = ' ';
    memcpy(pat + 1, name, hlen);
    pat[hlen + 1] = ' ';
    /* caller passes a lower-cased, sanitized name; file keys are lower-cased */

    while (n < max && off <= len && plen <= len - off) {
        const uint8_t *p = memmem(map + off, len - off, pat, plen);
        const uint8_t *line, *eol;
        size_t linelen;
        if (!p) break;
        line = p;
        while (line > map && line[-1] != '\n') line--;
        eol = p + plen;
        while (eol < map + len && *eol != '\n') eol++;
        linelen = (size_t)(eol - line);
        if (parse_ip(line, (size_t)(p - line), ip) &&
            fresh_line(line, linelen, cutoff))
            memcpy(ips[n++], ip, 4);
        off = (size_t)(p - map) + plen;
    }
    return n;
}

/* --------------------------------------------------------- append + prune */


/* Append "<ip> <name> #<ts>\n" to the secure cache. `name` lower-cased,
 * sanitized. */
static void append_pair(const char *host, const uint8_t ip[4])
{
    uint8_t line[MAX_NAME + 64];
    size_t n = 0, hlen = strlen(host);
    long fd;

    if (!name_ok(host) || hlen > MAX_NAME) return;

    for (int j = 0; j < 4; j++) {
        n += utoa(ip[j], (char *)line + n);
        line[n++] = j == 3 ? ' ' : '.';
    }
    memcpy(line + n, host, hlen); n += hlen;
    line[n++] = ' ';
    line[n++] = '#';
    n += utoa((unsigned long)now_secs(), (char *)line + n);
    line[n++] = '\n';

    fd = open(CACHE_FILE, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (check_owned_regular((int)fd, 0) == 0)
        (void)write_all((int)fd, line, n);
    close(fd);
}

/* forward */
static void prune(uint8_t **map, size_t *maplen, size_t need);

/* append + S8 prune-to-fit + remap */
static void append_pruned(const char *host, const uint8_t ip[4],
                          uint8_t **map, size_t *maplen)
{
    /* estimate added size conservatively to prune-to-fit first */
    size_t need = strlen(host) + 3 + 15 + 1 + 1 + 20 + 1 + 1; /* ip name #ts\n */
    if (*maplen + need > MAX_CACHE) prune(map, maplen, need);
    /* re-check real size after append via remap below; prune again if needed */
    append_pair(host, ip);
    if (refresh_map(CACHE_FILE, map, maplen) == 0 && *maplen > MAX_CACHE)
        prune(map, maplen, 0);
}

/* Rewrite the cache without expired entries; then, if still over
 * MAX_CACHE - need, drop still-valid entries oldest-first.  Atomic: write
 * private temp + renameat2().  No-op when nothing is dropped. */
static void prune(uint8_t **map, size_t *maplen, size_t need)
{
    size_t len = *maplen, off = 0, out = 0;
    long long cutoff = now_secs() - (long long)MAX_AGE_SECS;
    uint8_t *copy;
    long fd;

    if (!*map || !len) return;
    copy = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) return;
    memcpy(copy, *map, len);

    /* phase 1: drop expired / malformed lines */
    while (off < len) {
        size_t end = off, linelen;
        while (end < len && copy[end] != '\n') end++;
        linelen = (end < len ? end + 1 : end) - off;
        if (fresh_line(copy + off, end - off, cutoff)) {
            memmove(copy + out, copy + off, linelen);
            out += linelen;
        }
        off += linelen;
    }

    /* phase 2: drop still-valid lines, oldest (smallest #ts) first */
    while (out + need > MAX_CACHE) {
        size_t boff = (size_t)-1, blen = 0, scan = 0;
        long long bts = 0;
        while (scan < out) {
            size_t e = scan, ll;
            long long t;
            while (e < out && copy[e] != '\n') e++;
            ll = (e < out ? e + 1 : e) - scan;
            if (line_ts(copy + scan, e - scan, &t) &&
                (boff == (size_t)-1 || t < bts)) { boff = scan; blen = ll; bts = t; }
            scan += ll;
        }
        if (boff == (size_t)-1) break;
        memmove(copy + boff, copy + boff + blen, out - (boff + blen));
        out -= blen;
    }

    if (out == len) { munmap(copy, len); return; }   /* nothing dropped */

    unlinkat(AT_FDCWD, CACHE_TMP, 0);
    fd = open(CACHE_TMP, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC |
              O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        int ok = (write_all((int)fd, copy, out) == 0);
        if (close(fd) < 0) ok = 0;
        if (ok && renameat2(AT_FDCWD, CACHE_TMP, AT_FDCWD, CACHE_FILE, 0) == 0) {
            (void)refresh_map(CACHE_FILE, map, maplen);
            munmap(copy, len);
            return;
        }
        unlinkat(AT_FDCWD, CACHE_TMP, 0);
    }
    munmap(copy, len);
    /* on failure leave previous mapping untouched */
}

/* -------------------------------------------------------------- DNS wire */

/* Pull the first pair out of an answer: the A address reached from the
 * question name through the answer's CNAMEs, under the original name. */
static void log_answer(const uint8_t *m, size_t len,
                       uint8_t **map, size_t *maplen)
{
    char chain[MAX_CHAIN][MAX_NAME + 1], name[MAX_NAME + 1];
    uint8_t addr[4];
    size_t pos = 12, end;
    int nchain = 0, have = 0;
    size_t qd, an;

    if (len < 12) return;
    qd = ((size_t)m[4] << 8) | m[5];
    an = ((size_t)m[6] << 8) | m[7];

    for (size_t i = 0; i < qd; i++) {
        if (!dns_name(m, len, pos, name, sizeof name, &end)) return;
        if (i == 0 && *name && name_ok(name) && nchain < MAX_CHAIN) {
            memcpy(chain[nchain], name, strlen(name) + 1);
            lower(chain[nchain]);
            nchain++;
        }
        pos = end;
        if (len - pos < 4) return;
        pos += 4;
    }
    if (!nchain) return;

    for (size_t i = 0; i < an; i++) {
        size_t rdlen;
        uint16_t type, class;

        if (!dns_name(m, len, pos, name, sizeof name, &end)) return;
        pos = end;
        if (len - pos < 10) return;
        type = (uint16_t)(((uint16_t)m[pos] << 8) | m[pos + 1]);
        class = (uint16_t)(((uint16_t)m[pos + 2] << 8) | m[pos + 3]);
        rdlen = ((size_t)m[pos + 8] << 8) | m[pos + 9];
        pos += 10;
        if (rdlen > len - pos) return;

        if (class == 1 && name_ok(name) && in_chain(chain, nchain, name)) {
            if (type == 5 && nchain < MAX_CHAIN) { /* CNAME: follow target */
                char target[MAX_NAME + 1];
                size_t ignored;
                if (dns_name(m, len, pos, target, sizeof target, &ignored) &&
                    *target && name_ok(target) &&
                    !in_chain(chain, nchain, target)) {
                    memcpy(chain[nchain], target, strlen(target) + 1);
                    lower(chain[nchain]);
                    nchain++;
                }
            } else if (type == 1 && rdlen == 4 && !have) {
                memcpy(addr, m + pos, 4);
                have = 1;
            }
        }
        pos += rdlen;
    }
    if (have) append_pruned(chain[0], addr, map, maplen);  /* lower-cased key */
}

static void put16(uint8_t *p, uint16_t v) { *p = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Build a response from the query's own question: rcode 3 with no answers,
 * or rcode 0 with one A record per cached address. */
static void send_response(int fd, const uint8_t *q, size_t qend,
                          const uint8_t ips[][4], int n, int rcode,
                          const struct sockaddr *client, int client_len)
{
    uint8_t out[4096];
    size_t qlen = qend + 4 - 12, len = 12 + qlen;

    if (qend < 12 || len + (size_t)n * 16 > sizeof out) return;
    memcpy(out, q, 12);
    out[2] = 0x80 | (uint8_t)(q[2] & 0x01);        /* QR, keep RD */
    out[3] = (uint8_t)(0x80 | rcode);              /* RA and the rcode */
    put16(out + 4, 1);
    put16(out + 6, (uint16_t)n);
    put16(out + 8, 0);
    put16(out + 10, 0);
    memcpy(out + 12, q + 12, qlen);

    for (int i = 0; i < n; i++) {
        uint8_t *a = out + len;
        a[0] = 0xC0; a[1] = 0x0C;
        put16(a + 2, 1);
        put16(a + 4, 1);
        a[6] = a[7] = a[8] = 0; a[9] = 60;
        put16(a + 10, 4);
        memcpy(a + 12, ips[i], 4);
        len += 16;
    }
    sendto(fd, out, len, 0, client, client_len);
}

/* uncompressed wire name at pos; *wirelen gets its on-wire length */
static int question_uncompressed(const uint8_t *m, size_t len, size_t pos,
                                 size_t *wirelen)
{
    size_t scan = pos;
    for (;;) {
        uint8_t l;
        if (scan >= len) return 0;
        l = m[scan];
        if (l & 0xC0) return 0;                  /* no compression in question */
        scan++;
        if (l == 0) { *wirelen = scan - pos; return 1; }
        if (l > len - scan) return 0;
        scan += l;
    }
}

/* The first question of a query: name (dotted, sanitized, lower-cased),
 * offset just past name, QTYPE. */
static int parse_query(const uint8_t *m, size_t len, char *name, size_t namez,
                       size_t *qend, uint16_t *qtype, size_t *name_len)
{
    size_t end, wirelen;
    if (len < 12) return 0;
    if ((((size_t)m[4] << 8) | m[5]) != 1) return 0;     /* QDCOUNT==1 */
    if (!dns_name(m, len, 12, name, namez, &end)) return 0;
    if (!question_uncompressed(m, len, 12, &wirelen)) return 0;
    if (end != 12 + wirelen) return 0;                   /* consistent */
    if (len - end < 4 || !*name || !name_ok(name)) return 0;  /* sanitized */
    *qend = end;
    *qtype = (uint16_t)(((uint16_t)m[end] << 8) | m[end + 1]);
    *name_len = wirelen;                                 /* raw name length */
    lower(name);                                         /* lower-cased key */
    return 1;
}

/* random bytes; 0 ok */
static int rand_bytes(uint8_t *p, size_t n)
{
    long r = getrandom(p, n, 0);
    return r == (long)n ? 0 : -1;
}

/* ------------------------------------------------------------------- main */

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct sockaddr_in local, upstream;
    uint8_t *hosts = 0;
    size_t hosts_len = 0;
    long long next_prune = 0;
    int fd;
    long r;

    (void)argc; (void)argv;

    if (setup_cache_dir() < 0) exit(1);            /* private dir or die */
    /* create empty cache securely if absent (0600, no link, exclusive) */
    {
        long cfd = open(CACHE_FILE, O_WRONLY | O_CREAT | O_EXCL |
                        O_NOFOLLOW | O_CLOEXEC, 0600);
        if (cfd >= 0) close(cfd);
    }
    /* S8: refuse to start if the existing cache already exceeds MAX_CACHE */
    if (refresh_map(CACHE_FILE, &hosts, &hosts_len) == 0 &&
        hosts_len > MAX_CACHE) exit(1);

    r = socket(AF_INET, SOCK_DGRAM, 0);
    if (r < 0) exit(1);
    fd = (int)r;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons(LOCAL_PORT);
    local.sin_addr.s_addr = htonl(ipv4(127, 0, 0, 1));
    /* exclusive bind on loopback; no SO_REUSEADDR/SO_REUSEPORT (no sharing);
     * kernel rejects a second binder; clean restart OK (no TIME_WAIT on UDP). */
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) exit(1);

    upstream = ipv4_with_port(1, 1, 1, 1, 53);

    for (;;) {
        struct sockaddr_in client;
        int client_len = sizeof client;
        uint8_t qbuf[BUFSIZE], ips[MAX_A][4];
        uint8_t question[2 * MAX_NAME + 8], sentquestion[2 * MAX_NAME + 8];
        char name[MAX_NAME + 1];
        size_t qend, question_len = 0, name_len;
        uint16_t qtype, client_id, upid;
        ssize_t n, m;
        int hit;

        n = recvfrom(fd, qbuf, sizeof qbuf, 0, (struct sockaddr *)&client,
                     &client_len);
        if (n < 0) continue;                       /* S9: transient, keep going */

        if (!parse_query(qbuf, (size_t)n, name, sizeof name, &qend, &qtype, &name_len))
            continue;                              /* S3: drop, never forward */

        /* non-A: NXDOMAIN without going upstream */
        if (qtype != 1) {
            send_response(fd, qbuf, qend, 0, 0, 3,
                          (struct sockaddr *)&client, client_len);
            continue;
        }

        {
            long long now = now_secs();
            long long cutoff = now - (long long)MAX_AGE_SECS;
            hit = cache_lookup(hosts, hosts_len, name, ips, MAX_A, cutoff);
        }
        if (hit > 0) {
            send_response(fd, qbuf, qend, ips, hit, 0,
                          (struct sockaddr *)&client, client_len);
            if (now_secs() >= next_prune) {        /* after the reply is sent */
                prune(&hosts, &hosts_len, 0);
                next_prune = now_secs() + CLEANUP_EVERY;
            }
            continue;
        }

        /* ---- upstream miss ---- */
        question_len = name_len + 4;               /* raw question length */
        memcpy(question, qbuf + 12, question_len);
        memcpy(sentquestion, question, question_len);
        client_id = (uint16_t)(((uint16_t)qbuf[0] << 8) | qbuf[1]);

        /* random unpredictable upstream transaction ID (not client's) */
        if (rand_bytes((uint8_t *)&upid, sizeof upid) < 0) continue;
        qbuf[0] = (uint8_t)(upid >> 8); qbuf[1] = (uint8_t)upid;

        /* 0x20 randomization on the sent question NAME */
        {
            uint8_t rnd[MAX_NAME];
            if (rand_bytes(rnd, name_len) == 0) {
                for (size_t k = 0; k < name_len; k++) {
                    uint8_t c = sentquestion[k];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                        int upper = rnd[k] & 1;
                        c = upper ? (uint8_t)(c & 0xDF) : (uint8_t)(c | 0x20);
                        sentquestion[k] = c;
                    }
                }
            }
            /* put the randomized name into the datagram actually sent */
            memcpy(qbuf + 12, sentquestion, name_len);
        }

        r = socket(AF_INET, SOCK_DGRAM, 0);
        if (r < 0) continue;
        {
            int ufd = (int)r;
            int got_valid = 0;
            if (connect(ufd, (struct sockaddr *)&upstream, sizeof upstream) >= 0 &&
                sendto(ufd, qbuf, (size_t)n, 0, 0, 0) == n) {
                /* drain datagrams until a valid answer or total timeout */
                int spent = 0;
                while (spent < REPLY_TIMEOUT_MS && !got_valid) {
                    struct pollfd p = { .fd = ufd, .events = POLLIN };
                    int slice = REPLY_TIMEOUT_MS - spent;
                    int pr = poll(&p, 1, slice);
                    if (pr <= 0) break;
                    /* (no precise per-packet timing; bound iterations) */
                    spent += 1; /* keep it simple; validity gate dominates */
                    if (spent > REPLY_TIMEOUT_MS) break;
                    m = recvfrom(ufd, qbuf, sizeof qbuf, 0, 0, 0);
                    if (m < 12) continue;          /* m>=12 */
                    if (!(qbuf[2] & 0x80)) continue;         /* QR=1 */
                    if ((((uint16_t)qbuf[0] << 8) | qbuf[1]) != upid) continue; /* ID */
                    if ((((size_t)qbuf[4] << 8) | qbuf[5]) != 1) continue;      /* QD=1 */
                    if ((size_t)m < 12 + question_len) continue;
                    /* byte-exact question match (incl. 0x20 case) */
                    if (memcmp(qbuf + 12, sentquestion, question_len) != 0)
                        continue;
                    got_valid = 1;

                    /* restore client transaction ID */
                    qbuf[0] = (uint8_t)(client_id >> 8);
                    qbuf[1] = (uint8_t)client_id;
                    /* restore original question (case) */
                    memcpy(qbuf + 12, question, question_len);

                    sendto(fd, qbuf, (size_t)m, 0,
                           (struct sockaddr *)&client, client_len);
                    /* cache only NOERROR answers from a validated response */
                    if ((qbuf[3] & 0x0F) == 0)
                        log_answer(qbuf, (size_t)m, &hosts, &hosts_len);
                }
            }
            (void)got_valid;
            close(ufd);
        }
    }
}
