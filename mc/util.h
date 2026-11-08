/* util.h - small helpers: strings, the environment, numbers, UTF-8 text and its
 * width in cells, paths, reading and writing whole buffers, running a tool. */

/* ------------------------------------------------------------ small libc */

static int scmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Value of environment variable NAME, or NULL. */
static const char *genv(const char *name)
{
    for (char **e = envp; *e; e++) {
        const char *s = *e;
        size_t i = 0;
        while (name[i] && name[i] == s[i]) i++;
        if (!name[i] && s[i] == '=') return s + i + 1;
    }
    return NULL;
}

/* Copy s to d, return its length. */
static int cat(char *d, const char *s)
{
    int n = 0;
    while (*s) d[n++] = *s++;
    return n;
}

/* Write V right-aligned, zero-free, into W characters of B. */
static void putnum(char *b, unsigned long v, int w)
{
    for (int i = w - 1; i >= 0; i--) {
        b[i] = (v || i == w - 1) ? (char)('0' + v % 10) : ' ';
        v /= 10;
    }
}

/* Write S centred in W characters of B. */
static void center(char *b, const char *s, int w)
{
    int l = (int)strlen(s), pad = (w - l) / 2;
    for (int i = 0; i < w; i++) b[i] = ' ';
    for (int i = 0; i < l && pad + i < w; i++) b[pad + i] = s[i];
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Read the input FD up to N bytes, unless it ends first: the number read. */
static size_t read_full(long fd, void *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        long r = read((int)fd, (char *)buf + got, n - got);

        if (r == -EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}

/* Writes all N bytes at P to FD: 0, or a negative errno. */
static long write_all(long fd, const uint8_t *p, size_t n)
{
    while (n) {
        long r = write((int)fd, p, n);

        if (r == -EINTR) continue;
        if (r < 0) return r;
        if (r == 0) return -EIO;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Zero-width codepoints, as vt.c has them: joiners, variation selectors and
 * zero-width spaces.  They are not drawn, and take no cells. */
static int zero_width(uint32_t cp)
{
    return cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x200B && cp <= 0x200F);
}

/* The cells a codepoint takes: two if width.h says it is double-width. */
static int cwidth(uint32_t cp)
{
    if (zero_width(cp)) return 0;
    return cjk_wide(cp) || emoji_wide(cp) ? 2 : 1;
}

/* The codepoint of the UTF-8 character at S[*I] (N bytes in all), which *I is
 * moved past.  A byte that does not start a whole character is U+FFFD. */
static uint32_t utf8_get(const char *s, int n, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    int k = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc0 && c < 0xe0 ? 1 : 0;
    uint32_t cp = k == 3 ? c & 0x07 : k == 2 ? c & 0x0f : k == 1 ? c & 0x1f : c;

    if (c < 0x80) {
        (*i)++;
        return c;
    }
    if (k == 0 || *i + k >= n) {
        (*i)++;
        return 0xfffd;
    }
    for (int j = 1; j <= k; j++) {
        unsigned char d = (unsigned char)s[*i + j];

        if ((d & 0xc0) != 0x80) {
            (*i)++;
            return 0xfffd;
        }
        cp = (cp << 6) | (d & 0x3f);
    }
    *i += k + 1;
    return cp;
}

/* The cells the N bytes of UTF-8 text at S take. */
static int tcols(const char *s, int n)
{
    int w = 0;

    for (int i = 0; i < n; ) w += cwidth(utf8_get(s, n, &i));
    return w;
}

/* The bytes of the longest beginning of the N bytes at S that takes at most MAX
 * cells, cut between characters. */
static int cut_cols(const char *s, int n, int max)
{
    int w = 0, i = 0;

    while (i < n) {
        int at = i, c = cwidth(utf8_get(s, n, &i));

        if (w + c > max) return at;
        w += c;
    }
    return n;
}

/* The UTF-8 bytes of the code point CP, into B (4 bytes at most); returns their number. */
static int utf8_enc(char *b, uint32_t cp)
{
    int n = 0;

    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xc0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        b[n++] = (char)(0xe0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        b[n++] = (char)(0xf0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    }
    return n;
}

/* NAME, in W cells: all of it if it fits, else its beginning and its end with a
 * ~ between, as mc truncates names: the beginning as far as W/2 cells, whole
 * characters only, and the end as far as the cells that are left.  Any cells
 * short of W are added at the end.  The bytes go to T; their number is returned. */
static int fit_name(char *t, const char *name, int w)
{
    int l = (int)strlen(name), i = 0, n = 0, cw = 0, total, head = w / 2, at, c, rest, before = 0, j = 0;

    if (w <= 0) return 0;
    total = tcols(name, l);
    if (total <= w) return cat(t, name);
    while (i < l) {
        at = i;
        c = cwidth(utf8_get(name, l, &i));
        if (cw + c > head) {
            i = at;
            break;
        }
        for (int k = at; k < i; k++) t[n++] = name[k];
        cw += c;
    }
    t[n++] = '~';
    cw++;
    rest = w - head - 1;
    while (total - before > rest) before += cwidth(utf8_get(name, l, &j));
    for (int k = j; k < l; k++) t[n++] = name[k];
    cw += total - before;
    while (cw < w) {
        t[n++] = ' ';
        cw++;
    }
    return n;
}

/* Collapse "." and ".." and repeated slashes in absolute path S into OUT. */
static void canon(char *out, int max, const char *s)
{
    int len = 1;
    const char *q = s;

    out[0] = '/';
    out[1] = 0;
    while (*q) {
        const char *c;
        size_t cl;

        while (*q == '/') q++;
        if (!*q) break;
        c = q;
        while (*q && *q != '/') q++;
        cl = (size_t)(q - c);
        if (cl == 1 && c[0] == '.') continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            out[len] = 0;
            continue;
        }
        if (len > 1 && len < max - 1) out[len++] = '/';
        for (size_t k = 0; k < cl && len < max - 1; k++) out[len++] = c[k];
        out[len] = 0;
    }
}

static const char *errstr(long e)
{
    switch (e) {
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "Input/output error";
    case 13: return "Permission denied";
    case 16: return "Device or resource busy";
    case 17: return "File exists";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
    case 30: return "Read-only file system";
    case 36: return "File name too long";
    case 39: return "Directory not empty";
    case 40: return "Too many levels of symbolic links";
    default: return "Unknown error";
    }
}

/* DIR and NAME into D (max bytes): a path. Returns 0, or -ENAMETOOLONG. */
static long join(char *d, size_t max, const char *dir, const char *name)
{
    size_t ld = strlen(dir), ln = strlen(name), k;
    int slash = ld && dir[ld - 1] != '/';

    if (ld + (slash ? 1 : 0) + ln + 1 > max) return -ENAMETOOLONG;
    memcpy(d, dir, ld);
    k = ld;
    if (slash) d[k++] = '/';
    memcpy(d + k, name, ln);
    d[k + ln] = 0;
    return 0;
}

/* Whether D lies inside the directory S. */
static int under(const char *d, const char *s)
{
    size_t l = strlen(s), k;

    if (strlen(d) <= l || d[l] != '/') return 0;
    for (k = 0; k < l; k++)
        if (d[k] != s[k]) return 0;
    return 1;
}

/* Whether two files are one file: the same device and inode. */
static int same_file(const struct statx *a, const struct statx *b)
{
    return a->stx_dev_major == b->stx_dev_major && a->stx_dev_minor == b->stx_dev_minor &&
           a->stx_ino == b->stx_ino;
}

/* Copy field FIELD (0 = name, 5 = home directory) of the /etc/passwd entry
 * whose name is NAME (NL bytes), or, when NAME is NULL, whose uid is WANT.
 * Returns 1 if there is such an entry. */
static int pw_field(const char *name, size_t nl, long want, int field, char *out, int max)
{
    static char pw[16384];
    static long pwn = -1;
    long i, e;

    if (pwn < 0) {
        long fd = open("/etc/passwd", O_RDONLY, 0), r;

        pwn = 0;
        if (fd >= 0) {
            while (pwn < (long)sizeof pw && (r = read((int)fd, pw + pwn, sizeof pw - pwn)) > 0)
                pwn += r;
            close((int)fd);
        }
    }
    for (i = 0; i < pwn; i = e + 1) {
        long fs[7], fe[7], k, s0 = i;
        int nf = 0;

        for (e = i; e < pwn && pw[e] != '\n'; e++) ;
        for (k = i; k <= e && nf < 7; k++)              /* split at the colons */
            if (k == e || pw[k] == ':') {
                fs[nf] = s0;
                fe[nf] = k;
                nf++;
                s0 = k + 1;
            }
        if (nf < 6) continue;

        if (name) {
            size_t j = 0;

            if ((size_t)(fe[0] - fs[0]) != nl) continue;
            while (j < nl && pw[fs[0] + (long)j] == name[j]) j++;
            if (j != nl) continue;
        } else {
            unsigned long v = 0;
            int digits = 0;

            for (k = fs[2]; k < fe[2] && pw[k] >= '0' && pw[k] <= '9'; k++) {
                v = v * 10 + (unsigned long)(pw[k] - '0');
                digits++;
            }
            if (!digits || k != fe[2] || (long)v != want) continue;
        }
        for (k = 0; fs[field] + k < fe[field] && k < max - 1; k++)
            out[k] = pw[fs[field] + k];
        out[k] = 0;
        return 1;
    }
    return 0;
}

static void user_name(void)
{
    if (!pw_field(NULL, 0, uid, 0, user, sizeof user))
        user[0] = 0;
}

/* Replace the process by TOOL, found on the PATH, with ARGV: it returns only if no
 * element of the PATH has it. */
static void exec_tool(const char *tool, char *const argv[])
{
    const char *path = genv("PATH");
    size_t tl = strlen(tool);
    char buf[4096];

    if (!path) path = "/usr/bin:/bin";
    for (;;) {
        const char *e = path;
        size_t dl, k;

        while (*e && *e != ':') e++;
        dl = (size_t)(e - path);
        if (dl + tl + 2 <= sizeof buf) {
            for (k = 0; k < dl; k++) buf[k] = path[k];
            if (dl == 0) buf[k++] = '.';        /* an empty element is the current directory */
            buf[k++] = '/';
            for (size_t j = 0; j < tl; j++) buf[k++] = tool[j];
            buf[k] = 0;
            execve(buf, argv, envp);
        }
        if (!*e) return;
        path = e + 1;
    }
}

/* Zero a block without a memcpy call (see tcopy). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void zero_block(void *p, size_t n)
{
    uint8_t *d = (uint8_t *)p;

    for (size_t i = 0; i < n; i++) d[i] = 0;
}

/* A new process, a copy of this one. In it the signals mc sets aside (SIGINT and
 * SIGQUIT while a program runs, SIGPIPE for the link) are as they are by default:
 * an ignored signal stays ignored through exec, and what runs must not inherit it.
 * The pid in the parent, 0 in the child, or -errno. */
static long fork_child(void)
{
    long pid = clone(SIGCHLD, 0, 0, 0, 0);

    if (pid == 0) {
        struct sigaction sa;

        zero_block(&sa, sizeof sa);
        sa._u._sa_handler = SIG_DFL;
        rt_sigaction(SIGINT, &sa, NULL, 8);
        rt_sigaction(SIGQUIT, &sa, NULL, 8);
        rt_sigaction(SIGPIPE, &sa, NULL, 8);
    }
    return pid;
}

/* Starts ARGV with its input from IN and its output to OUT (-1: /dev/null). Its pid,
 * or -errno. */
static long spawn_io(char *const argv[], int in, int out)
{
    long pid = fork_child();
    int dn;

    if (pid != 0) return pid;
    dn = (int)open("/dev/null", O_RDWR, 0);
    dup2(in >= 0 ? in : dn, 0);
    dup2(out >= 0 ? out : dn, 1);
    dup2(dn, 2);
    exec_tool(argv[0], argv);
    exit(127);
}

/* Waits for the process PID: its exit status, or -1 if it did not exit. */
static int wait_pid(long pid)
{
    int st = 0;
    long r;

    do r = wait4((int)pid, &st, 0, NULL);
    while (r == -EINTR);
    return r < 0 || (st & 0x7f) ? -1 : (st >> 8) & 0xff;
}
