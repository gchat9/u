/* ----------------------------------------------------------- shell links
 *
 * A shell link, sh://user@host/dir, shows a directory on another machine. It is
 * reached by ssh: the shell there, started as "echo SHELL:;/bin/sh", reads commands
 * from ssh's input and answers on its output, each answer ending with a "### NNN"
 * line (the protocol of mc's helpers/README.shell). One link is open at a time, and
 * a panel on it has rem set, its path being the directory there. The local directory
 * the link was opened from is rorig: going up from the link's root goes back there. */


/* The next line from ssh into L (MAX bytes, the newline dropped): its length, or -1
 * at the end of its output. */
static int rline(char *l, int max)
{
    int k = 0;

    for (;;) {
        char c;

        if (rpos == rn) {
            long r = read(rr, rb, sizeof rb);

            if (r == -EINTR) continue;
            if (r <= 0) return -1;
            rn = (int)r;
            rpos = 0;
        }
        c = rb[rpos++];
        if (c == '\n') break;
        if (k < max - 1) l[k++] = c;
    }
    l[k] = 0;
    return k;
}

/* Writes S (N bytes) to ssh. 0, or a negative errno. */
static int rsend(const char *s, size_t n)
{
    while (n) {
        long w = write(rw, s, n);

        if (w == -EINTR) continue;
        if (w < 0) return (int)w;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Sends the script CMD and reads its answer: each line but the last goes to FN, with
 * X, if not NULL; the last, "### NNN", gives the code NNN. A negative errno if ssh
 * is gone. */
static int rrun(const char *cmd, void (*fn)(void *x, const char *line), void *x)
{
    char l[1024];
    int r;

    if (rr < 0) return -ENOTCONN;
    r = rsend(cmd, strlen(cmd));
    if (r == 0) r = rsend("\n", 1);
    if (r) return r;
    for (;;) {
        int k = rline(l, sizeof l), c = 0;

        if (k < 0) return -EIO;
        if (l[0] == '#' && l[1] == '#' && l[2] == '#' && l[3] == ' ') {
            for (const char *q = l + 4; *q >= '0' && *q <= '9'; q++) c = c * 10 + (*q - '0');
            return c;
        }
        if (fn) fn(x, l);
    }
}

/* Takes the link down: its input closes, ssh ends, and it is waited for. */
static void rclose(void)
{
    int st;

    if (rw >= 0) close(rw);
    if (rr >= 0) close(rr);
    if (rpid > 0)
        while (wait4((int)rpid, &st, 0, NULL) == -EINTR)
            ;
    rw = rr = -1;
    rpid = -1;
    rhost[0] = 0;
    rn = rpos = 0;
}

/* Takes the link down, when no panel is on it. */
static void rdrop(void)
{
    struct panel *a = &P[0], *b = &P[1];

    if (!a->rem && !b->rem) rclose();
}

/* Starts ssh to HOST (user@host), and reads the remote shell's answer, "SHELL:". 0,
 * or a negative errno. */
static int rconnect(const char *host)
{
    struct sigaction sa;
    int a[2], b[2], dn;
    long pid;
    char *argv[8], l[256];

    if (host[0] == '-') return -EINVAL;      /* no option: the host is an argument */
    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = SIG_IGN;             /* ssh's end is not ours to stop on */
    rt_sigaction(SIGPIPE, &sa, NULL, 8);
    if (pipe2(a, O_CLOEXEC) < 0) return -EMFILE;
    if (pipe2(b, O_CLOEXEC) < 0) {
        close(a[0]);
        close(a[1]);
        return -EMFILE;
    }
    pid = fork_child();
    if (pid < 0) {
        close(a[0]);
        close(a[1]);
        close(b[0]);
        close(b[1]);
        return (int)pid;
    }
    if (pid == 0) {
        dn = (int)open("/dev/null", O_RDWR, 0);
        dup2(a[0], 0);
        dup2(b[1], 1);
        if (dn >= 0) dup2(dn, 2);
        argv[0] = (char *)"ssh";
        argv[1] = (char *)"-o";
        argv[2] = (char *)"BatchMode=yes";
        argv[3] = (char *)"-o";
        argv[4] = (char *)"ConnectTimeout=20";
        argv[5] = (char *)host;
        argv[6] = (char *)"echo SHELL:;/bin/sh";
        argv[7] = NULL;
        exec_tool("ssh", argv);
        exit(127);
    }
    close(a[0]);
    close(b[1]);
    rw = a[1];
    rr = b[0];
    rpid = pid;
    rn = rpos = 0;
    for (;;) {
        int k = rline(l, sizeof l);

        if (k < 0) {
            rclose();
            return -EIO;
        }
        if (scmp(l, "SHELL:") == 0) return 0;
    }
}

/* S quoted for the shell, into Q: 'S', with each ' written as '\''. 0, or -1 if it
 * does not fit. */
static int rquote(char *q, size_t max, const char *s)
{
    size_t k = 0;

    if (max < 3) return -1;
    q[k++] = '\'';
    for (; *s; s++) {
        int esc = *s == '\'';

        if (k + (esc ? 4 : 1) + 2 > max) return -1;
        if (esc) {
            q[k++] = '\'';
            q[k++] = '\\';
            q[k++] = '\'';
            q[k++] = '\'';
        } else {
            q[k++] = *s;
        }
    }
    q[k++] = '\'';
    q[k] = 0;
    return 0;
}

/* A number at *Q: its digits, which *Q moves past. */
static long rnum(const char **q)
{
    long v = 0;

    while (**q >= '0' && **q <= '9') v = v * 10 + (*(*q)++ - '0');
    return v;
}

/* One line of find -printf '%y %Y %m %s %T@ %f': the type of the entry and of its
 * target (for a link), the mode, size, time and name, as in the line. 0, or -1 if the
 * line is not one. */
struct rstat { const char *name; uint64_t size; long mtime; int y, Y; unsigned mode; };

static int rfields(const char *l, struct rstat *r)
{
    const char *q = l + 4;

    if (!l[0] || l[1] != ' ' || l[3] != ' ') return -1;
    r->y = l[0];
    r->Y = l[2];
    r->mode = 0;
    while (*q >= '0' && *q <= '7') r->mode = r->mode * 8 + (unsigned)(*q++ - '0');
    if (*q++ != ' ') return -1;
    r->size = (uint64_t)rnum(&q);
    if (*q++ != ' ') return -1;
    r->mtime = rnum(&q);
    if (*q == '.') {
        q++;
        rnum(&q);
    }
    if (*q++ != ' ' || !*q) return -1;
    r->name = q;
    return 0;
}

/* One entry of the listing: appended to the panel P. A link is shown as its target
 * is: a link to a directory with the directories, a link to a file with the files,
 * a dangling one as stale. */
static void rent(void *x, const char *l)
{
    struct panel *p = x;
    struct rstat rs;
    struct ent *e;
    int at;

    if (p->ne >= MAXE || rfields(l, &rs) != 0) return;
    at = add_name(p, rs.name);
    if (at < 0) return;
    e = &p->e[p->ne++];
    e->name = (uint32_t)at;
    e->up = 0;
    e->sel = 0;
    e->link = rs.y == 'l';
    if (rs.y == 'l') e->cls = rs.Y == 'd' ? CL_DIR : (rs.Y == 'N' || rs.Y == 'L') ? CL_STALE : CL_NORMAL;
    else if (rs.y == 'd') e->cls = CL_DIR;
    else e->cls = (rs.mode & 0111) ? CL_EXE : CL_NORMAL;
    e->size = size_pack(rs.size);
    e->mtime = (uint32_t)rs.mtime;
}

/* The time of ".." : the answer to stat on the parent directory, as the number of
 * seconds. */
static void rup(void *x, const char *l)
{
    struct panel *p = x;
    const char *q = l;

    if (*q >= '0' && *q <= '9') p->e[0].mtime = (uint32_t)rnum(&q);
}

/* Reads the directory of P on the link, with ls in long form, and lists it. 0, or a
 * negative errno; the listing is then only "..". */
static int rload(struct panel *p)
{
    char q[4096 + 64], cmd[4096 + 160];
    struct ent *e = &p->e[0];
    int rc;

    p->nn = 0;
    add_name(p, "..");
    e->name = 0;
    e->size = 0;
    e->cls = CL_DIR;
    e->up = 1;
    e->sel = 0;
    e->link = 0;
    e->mtime = 0;
    p->ne = 1;
    p->free_kb = p->total_kb = 0;
    if (rquote(q, sizeof q, p->path) < 0) return -ENAMETOOLONG;
    strlcpy(cmd, "find -H ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " -mindepth 1 -maxdepth 1 -printf '%y %Y %m %s %T@ %f\\n' 2>/dev/null"
                 " && echo '### 200' || echo '### 500'", sizeof cmd);
    rc = rrun(cmd, rent, p);
    if (rc == 200 && rquote(q, sizeof q, p->path) == 0) {    /* the time of ".." */
        strlcpy(cmd, "stat -c %Y -- ", sizeof cmd);
        strlcat(cmd, q, sizeof cmd);
        cmd[strlen(cmd) - 1] = 0;                 /* the quote closes after the /.. */
        strlcat(cmd, "/..' 2>/dev/null; echo '### 200'", sizeof cmd);
        rrun(cmd, rup, p);
    }
    sort_list(p, NULL);
    if (rc == 200) return 0;
    if (rc == 500) return -ENOENT;
    return rc < 0 ? rc : -EIO;
}

static int shell_connect(struct panel *p, const char *url, char *dir, size_t max)
{
    const struct panel *o = &P[(p - P) ^ 1];
    const char *h = url + 5, *d = h;
    char host[256];
    size_t hl;
    int r;

    while (*d && *d != '/') d++;
    hl = (size_t)(d - h);
    if (hl == 0 || hl >= sizeof host) {
        set_error("Bad shell link", url, EINVAL);
        return -EINVAL;
    }
    memcpy(host, h, hl);
    host[hl] = 0;
    strlcpy(dir, *d ? d : "/", max);
    if (o->rem && scmp(rhost, host) != 0) {
        set_error("One shell link at a time", url, EBUSY);
        return -EBUSY;
    }
    if (rr < 0 || scmp(rhost, host) != 0) {
        rclose();
        r = rconnect(host);
        if (r) {
            set_error("Cannot connect to", host, -r);
            return r;
        }
        strlcpy(rhost, host, sizeof rhost);
    }
    return 0;
}

/* One command of its own on the link, by ssh, started: its input from IN and its output
 * to OUT (-1 for none). Its pid, or -errno. */
static long rssh_start(const char *cmd, int in, int out)
{
    char *argv[8];

    argv[0] = (char *)"ssh";
    argv[1] = (char *)"-o";
    argv[2] = (char *)"BatchMode=yes";
    argv[3] = (char *)"-o";
    argv[4] = (char *)"ConnectTimeout=20";
    argv[5] = rhost;
    argv[6] = (char *)cmd;
    argv[7] = NULL;
    return spawn_io(argv, in, out);
}

/* That command run to its end: its exit status, or -1. */
static int rssh(const char *cmd, int in, int out)
{
    long pid = rssh_start(cmd, in, out);

    return pid < 0 ? -1 : wait_pid(pid);
}
