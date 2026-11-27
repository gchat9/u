#define EXPORT_IMPLEMENTATIONS 1
#include "../_sys/_.h"
#include "../_lib/errstr.h"
#include "sodium.h"
#include "resolve.h"

/* The errno of a RES_SYSERR (a positive number), for resolve_strerror(). */
static int sys_errno;

/* The next word of a line, split at blanks, tabs and CRs (what strtok_r did
 * with these delimiters): NULL when there is none.  *p is where to go on. */
static char *next_word(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (!*s) { *p = s; return 0; }
    char *w = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++;
    if (*s) *s++ = '\0';
    *p = s;
    return w;
}

/* ═══════════════════════════════ Files ═══════════════════════════════════ */

/* Calls fn(line, ctx) for each line of a file, the line end in the line being
 * a NUL, until fn returns not 0, which is returned.  0 at the end of the file,
 * and if there is none (or it cannot be read: as it was empty).  The file is
 * read in pieces of 2 KiB, and a line that does not fit in one is skipped
 * whole: no line of /etc/hosts or /etc/resolv.conf that has a use is as long. */
static int each_line(const char *path, int (*fn)(char *, void *), void *ctx)
{
    int fd = open(path, O_RDONLY, 0);
    char buf[2048];
    size_t have = 0;
    int skip = 0, v = 0;
    if (fd < 0) return 0;
    for (;;) {
        ssize_t r = read(fd, buf + have, sizeof buf - 1 - have);
        if (r == -EINTR) continue;
        if (r <= 0) {                       /* the end, or something that is read as the end */
            if (r == 0 && have && !skip) { buf[have] = '\0'; v = fn(buf, ctx); }
            break;
        }
        have += (size_t)r;
        char *p = buf, *end = buf + have, *nl;
        while (!v && (nl = memchr(p, '\n', (size_t)(end - p)))) {
            *nl = '\0';
            if (!skip) v = fn(p, ctx);
            skip = 0;                       /* a skipped line ends here */
            p = nl + 1;
        }
        if (v) break;
        have = (size_t)(end - p);
        if (skip || have == sizeof buf - 1) { skip = 1; have = 0; }
        else memmove(buf, p, have);
    }
    close(fd);
    return v;
}

/* Four decimal numbers, 0 to 255, with no leading zero but 0 itself, and dots.
 * a is written only if s is one: a caller's default (127.0.0.1, say) is kept
 * when a line of the files is not an address. */
static int dotted_quad(const char *s, uint8_t a[4])
{
    uint8_t t[4];
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        const char *d = s;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; if (v > 255) return 0; }
        if (s == d || (s - d > 1 && *d == '0')) return 0;
        t[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s != '\0') return 0;
    memcpy(a, t, 4);
    return 1;
}

/* ═════════════════════════════ /etc/hosts ════════════════════════════════ */

struct hosts_q { const char *name; uint8_t (*addr)[4]; int max, n; };

/* "address name [alias ...]", with a # for a comment. */
static int hosts_line(char *line, void *ctx)
{
    struct hosts_q *q = ctx;
    uint8_t a[4];
    char *p = line, *t, *h = strchr(line, '#');
    if (h) *h = '\0';
    t = next_word(&p);
    if (!t || !dotted_quad(t, a)) return 0;             /* (the lines of IPv6 are not for this) */
    while ((t = next_word(&p)))
        if (strcasecmp(t, q->name) == 0) {
            memcpy(q->addr[q->n++], a, 4);      /* (n < max: the line that makes it max is the last) */
            break;
        }
    return q->n >= q->max;
}

/* ═══════════════════════════ /etc/resolv.conf ════════════════════════════ */

/* "nameserver address": the first with an IPv4 address (an IPv6 one is passed over). */
static int resolv_line(char *line, void *ctx)
{
    char *p = line, *t = next_word(&p);
    if (!t || strcmp(t, "nameserver") != 0) return 0;
    t = next_word(&p);
    return t && dotted_quad(t, ctx);
}

/* ═══════════════════════════════ The packets ═════════════════════════════ */

/* RFC 1035: a header of 12 bytes, and the question: the name as labels (each a
 * length and the bytes, then a 0), the type (A is 1) and the class (IN is 1). */
size_t dns_query(uint8_t *buf, const char *name, uint16_t id)
{
    size_t o = 12;
    const char *p = name;
    if (!*p) return 0;
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8); buf[1] = (uint8_t)id;
    buf[2] = 0x01;                                      /* RD: recursion is wanted */
    buf[5] = 1;                                         /* one question */
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63) return 0;                 /* "a..b", ".a", a label too long */
        if (o - 12 + 1 + l + 1 > 255) return 0;         /* the name, with its 0, is at most 255 */
        buf[o++] = (uint8_t)l;
        memcpy(buf + o, p, l); o += l;
        p += l;
        if (*p == '.') p++;                             /* (a dot at the end ends the name) */
    }
    buf[o++] = 0;
    buf[o++] = 0; buf[o++] = 1;                         /* type A */
    buf[o++] = 0; buf[o++] = 1;                         /* class IN */
    return o;
}

/* The same letter, in the case that DNS does not tell apart: for the labels of the question.  (The
 * lengths are under 64, so they are as they are, and so are the type and the class.) */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/*
 * Not the answer to this query: RES_IGNORE.  It is, if it has the ID of the query, is a response,
 * and has our question in it, the same as we sent (as to the case of the letters).  Then it is an
 * answer: for an error or what is not to be understood, RES_NOTFOUND or RES_SERVFAIL; or the
 * addresses that are in the answer section, which is walked by its lengths: a name is a few labels
 * and a 0, or a pointer of 2 bytes, which ends it, and is not followed, so that no packet can
 * make it go round.  Nothing is read that is not in the packet.
 */
int dns_answer(const uint8_t *m, size_t len, const uint8_t *q, size_t ql,
               uint8_t addr[][4], int max)
{
    size_t pos, an;
    int n = 0;
    if (len < 12 || ql < 12 || m[0] != q[0] || m[1] != q[1]) return RES_IGNORE;
    if (!(m[2] & 0x80) || m[4] != 0 || m[5] != 1) return RES_IGNORE;    /* not a response; not one question */
    if (len < ql) return RES_IGNORE;
    for (pos = 12; pos < ql; pos++)
        if (lc(m[pos]) != lc(q[pos])) return RES_IGNORE;
    if (m[2] & 0x78) return RES_SERVFAIL;                               /* opcode of a response is 0 */
    if ((m[3] & 15) == 3) return RES_NOTFOUND;                          /* NXDOMAIN */
    if (m[3] & 15) return RES_SERVFAIL;
    for (an = (size_t)(m[6] << 8 | m[7]); an; an--) {
        size_t type, cls, rdlen;
        for (;;) {                                                      /* the name of the record */
            if (pos >= len) return RES_SERVFAIL;
            if (m[pos] == 0) { pos++; break; }
            if ((m[pos] & 0xC0) == 0xC0) { pos += 2; break; }           /* a pointer: 2 bytes, and the end */
            if (m[pos] & 0xC0) return RES_SERVFAIL;
            pos += 1 + (size_t)m[pos];
        }
        if (pos > len || len - pos < 10) return RES_SERVFAIL;           /* type, class, TTL, length */
        type = (size_t)(m[pos] << 8 | m[pos + 1]);
        cls = (size_t)(m[pos + 2] << 8 | m[pos + 3]);
        rdlen = (size_t)(m[pos + 8] << 8 | m[pos + 9]);
        pos += 10;
        if (rdlen > len - pos) return RES_SERVFAIL;
        if (type == 1 && cls == 1 && rdlen == 4 && n < max) memcpy(addr[n++], m + pos, 4);
        pos += rdlen;
    }
    if (n == 0 && (m[2] & 0x02)) return RES_SERVFAIL;                   /* cut, and no address in what is there */
    return n;
}

/* ═══════════════════════════════ The lookup ══════════════════════════════ */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One query, three times at the most, to the nameserver. */
static int ask(const uint8_t ns[4], int port, const uint8_t *q, size_t ql,
               uint8_t addr[][4], int max, int budget_ms)
{
    struct sockaddr_in sa;
    int fd, attempt, each = budget_ms > 0 ? budget_ms / 3 : 2000, rc = RES_TIMEOUT;
    if (each > 2000) each = 2000;
    if (each < 1) each = 1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    memcpy(&sa.sin_addr, ns, 4);
    long e = socket(AF_INET, SOCK_DGRAM, 0);
    if (e < 0) { sys_errno = (int)-e; return RES_SYSERR; }
    fd = (int)e;
    /* connect(): the kernel then takes nothing that comes from another address or port. */
    if ((e = connect(fd, (struct sockaddr *)&sa, sizeof sa)) < 0) { sys_errno = (int)-e; rc = RES_SYSERR; goto out; }
    for (attempt = 0; attempt < 3; attempt++) {
        long long end = now_ms() + each;
        if ((e = sendto(fd, q, ql, 0, 0, 0)) < 0) { sys_errno = (int)-e; rc = RES_SYSERR; goto out; }
        for (;;) {
            uint8_t buf[1500];
            long long left = end - now_ms();
            struct pollfd p = { .fd = fd, .events = POLLIN };
            ssize_t r;
            int pr;
            if (left <= 0) break;
            pr = poll(&p, 1, (int)left);
            if (pr == -EINTR) continue;
            if (pr < 0) { sys_errno = -pr; rc = RES_SYSERR; goto out; }
            if (pr == 0) break;
            r = recv(fd, buf, sizeof buf, 0);
            if (r < 0) {                                         /* nobody listens: ICMP, ECONNREFUSED */
                if (r == -EINTR) continue;
                sys_errno = (int)-r; rc = RES_SYSERR; goto out;
            }
            rc = dns_answer(buf, (size_t)r, q, ql, addr, max);
            if (rc == RES_IGNORE) { rc = RES_TIMEOUT; continue; }
            goto out;
        }
    }
out:
    close(fd);
    return rc;
}

int resolve4_with(const char *name, uint8_t addr[][4], int max, int budget_ms,
                  const char *hosts_path, const char *resolv_path, int port)
{
    struct hosts_q hq = { name, addr, max, 0 };
    uint8_t q[DNS_QUERY_MAX], ns[4] = { 127, 0, 0, 1 };
    uint16_t id;
    size_t ql;
    int rc;
    if (max < 1) return RES_BADNAME;
    if (dotted_quad(name, addr[0])) return 1;
    if (strchr(name, ':')) return RES_IPV6;
    each_line(hosts_path, hosts_line, &hq);
    if (hq.n) return hq.n;
    randombytes_buf(&id, sizeof id);
    ql = dns_query(q, name, id);
    if (!ql) return RES_BADNAME;
    each_line(resolv_path, resolv_line, ns);
    rc = ask(ns, port, q, ql, addr, max, budget_ms);
    return rc == 0 ? RES_NOTFOUND : rc;
}

int resolve4(const char *name, uint8_t addr[][4], int max, int budget_ms)
{
    return resolve4_with(name, addr, max, budget_ms, "/etc/hosts", "/etc/resolv.conf", 53);
}

const char *resolve_strerror(int code)
{
    switch (code) {
    case RES_BADNAME:  return "not a host name";
    case RES_IPV6:     return "IPv6 is not supported";
    case RES_NOTFOUND: return "no such host";
    case RES_TIMEOUT:  return "the nameserver does not answer";
    case RES_SERVFAIL: return "the nameserver's answer is an error";
    case RES_SYSERR:   return errstr(sys_errno);
    }
    return "?";
}
