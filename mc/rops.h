/* Whether PATH on the link passes the test of test(1), TEST being "-d" or "-e": 1 if it
 * does, 0 if not, -1 for an error. */
static int remote_test(const char *test, const char *path)
{
    char q[4096 + 64], cmd[4096 + 200];
    int rc;

    if (rr < 0 || rquote(q, sizeof q, path) < 0) return -1;
    strlcpy(cmd, "test ", sizeof cmd);
    strlcat(cmd, test, sizeof cmd);
    strlcat(cmd, " ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " && echo '### 200' || echo '### 500'", sizeof cmd);
    rc = rrun(cmd, NULL, NULL);
    return rc == 200 ? 1 : rc == 500 ? 0 : -1;
}

/* Whether PATH is a directory on the link (1), not one (0), or an error (-1). */
static int remote_isdir(const char *path) { return remote_test("-d", path); }

/* 0 if PATH is free on the link, so that something can be made there; -EEXIST if
 * it is not; -EIO if that is not known. */
static long remote_clash(const char *path)
{
    int e = remote_test("-e", path);

    return e == 0 ? 0 : e == 1 ? -EEXIST : -EIO;
}

/* Removes PATH on the link: a file, or an empty directory. A directory that is not
 * empty is an error, and nothing is removed from it: a recursive removal is not
 * asked for here. 0, or -errno. */
static long remote_rm(const char *path)
{
    char q[4096 + 64], cmd[4096 + 200];
    int rc;

    if (rr < 0 || rquote(q, sizeof q, path) < 0) return -EIO;
    strlcpy(cmd, "rm -- ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " 2>/dev/null || rmdir -- ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " 2>/dev/null && echo '### 200' || echo '### 500'", sizeof cmd);
    rc = rrun(cmd, NULL, NULL);
    return rc == 200 ? 0 : rc == 500 ? -ENOTEMPTY : -EIO;
}

/* The names of the items of P that a job works on (the marked ones, if SEL, else the
 * highlighted one) as a list of NUL-terminated names, written to the file FD. */
static long write_items(const struct panel *p, int sel, int fd)
{
    for (int d = 0; d < p->n; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        const char *nm = p->nm + e->name;
        size_t n;

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        n = strlen(nm) + 1;
        if (write(fd, nm, n) != (long)n) return -EIO;
    }
    return 0;
}

/* Fetches the file PATH of the link into the open file FD: the bytes, as cat gives them.
 * 0, or -errno. */
static long remote_fetch(const char *path, int fd)
{
    char q[4096 + 64], cmd[4096 + 200];

    if (rquote(q, sizeof q, path) < 0) return -ENAMETOOLONG;
    strlcpy(cmd, "cat -- ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    return rssh(cmd, -1, fd) == 0 ? 0 : -EIO;
}

/* Downloads the items of the link panel P (the marked ones, else the highlighted) into
 * the staging directory xo.root, as one tar stream: the stream comes through pump(),
 * so the bytes count, and it is unpacked there by a local tar. 0, or -errno. */
static long remote_stage(const struct panel *p, int sel)
{
    char list[4352], tar[4352], q[4096 + 64], cmd[4096 + 200], *argv[6];
    long r = xtmp_make(), pid;
    int lf = -1, tf = -1, fds[2];

    if (r) return r;
    if (join(list, sizeof list, xo.root, "list") || join(tar, sizeof tar, xo.root, "items.tar"))
        return -ENAMETOOLONG;
    lf = (int)open(list, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lf < 0) return lf;
    r = write_items(p, sel, lf);
    close(lf);
    if (r) return r;
    if (rquote(q, sizeof q, p->path) < 0) return -ENAMETOOLONG;
    strlcpy(cmd, "tar -C ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " -cf - --null -T -", sizeof cmd);
    lf = (int)open(list, O_RDONLY | O_CLOEXEC, 0);
    tf = (int)open(tar, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lf < 0 || tf < 0) return -EIO;
    if (pipe2(fds, O_CLOEXEC) < 0) return -EMFILE;
    pid = rssh_start(cmd, lf, fds[1]);
    close(fds[1]);
    r = pid < 0 ? pid : pump(fds[0], tf);
    close(fds[0]);
    if (pid >= 0 && wait_pid(pid) != 0 && r == 0) r = -EIO;
    close(lf);
    close(tf);
    if (!r) {
        argv[0] = (char *)"tar";
        argv[1] = (char *)"-xf";
        argv[2] = tar;
        argv[3] = (char *)"-C";
        argv[4] = xo.root;
        argv[5] = NULL;
        pid = spawn_io(argv, -1, -1);
        r = pid < 0 || wait_pid(pid) != 0 ? -EIO : 0;
    }
    unlinkat(AT_FDCWD, list, 0);
    unlinkat(AT_FDCWD, tar, 0);
    return r;
}

/* Uploads the items of the local panel P (the marked ones, else the highlighted) into
 * the directory DST of the link: a local tar's stream, through pump(), into a tar on
 * the link that unpacks it. Nothing that exists there is replaced; the items are then
 * removed here, if MOVE. 0, or -errno. */
static long local_upload(const struct panel *p, const char *dst, int sel, int move)
{
    char list[4352], q[4096 + 64], cmd[4096 + 200], *argv[10];
    int lf, a[2], b[2];
    long r = xtmp_make(), pt, ps;
    int st1, st2;

    if (r) return r;
    if (join(list, sizeof list, xo.root, "list")) return -ENAMETOOLONG;
    lf = (int)open(list, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lf < 0) return lf;
    r = write_items(p, sel, lf);
    close(lf);
    if (r) return r;
    for (int d = 0; d < p->n && !r; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        char target[4352];

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        if (join(target, sizeof target, dst, p->nm + e->name)) return -ENAMETOOLONG;
        r = remote_clash(target);
    }
    if (r) return r;
    if (rquote(q, sizeof q, dst) < 0) return -ENAMETOOLONG;
    strlcpy(cmd, "tar -C ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " -xf -", sizeof cmd);
    if (pipe2(a, O_CLOEXEC) < 0) return -EMFILE;
    if (pipe2(b, O_CLOEXEC) < 0) {
        close(a[0]);
        close(a[1]);
        return -EMFILE;
    }
    argv[0] = (char *)"tar";
    argv[1] = (char *)"-C";
    argv[2] = (char *)p->path;
    argv[3] = (char *)"-cf";
    argv[4] = (char *)"-";
    argv[5] = (char *)"--null";
    argv[6] = (char *)"-T";
    argv[7] = list;
    argv[8] = NULL;
    pt = spawn_io(argv, -1, a[1]);
    ps = rssh_start(cmd, b[0], -1);
    close(a[1]);
    close(b[0]);
    r = pt < 0 || ps < 0 ? -EIO : pump(a[0], b[1]);
    close(a[0]);
    close(b[1]);
    st1 = pt < 0 ? -1 : wait_pid(pt);
    st2 = ps < 0 ? -1 : wait_pid(ps);
    if (!r && (st1 != 0 || st2 != 0)) r = -EIO;
    unlinkat(AT_FDCWD, list, 0);
    if (!r && move)
        for (int d = 0; d < p->n; d++) {
            const struct ent *e = &p->e[p->ord[d]];
            char from[4352];

            if (e->up || (sel ? !e->sel : d != p->cur)) continue;
            if (join(from, sizeof from, p->path, p->nm + e->name)) return -ENAMETOOLONG;
            del_tree(from, 1);
        }
    return r;
}

/* A string into a buffer: 0, or -1 if it does not fit. */
static long str_fit(char *d, size_t max, const char *s)
{
    if (strlen(s) >= max) return -1;
    strlcpy(d, s, max);
    return 0;
}

/* The entry NAME of the directory SRC of the link, copied into the directory DST of the
 * link: a tar on the link, its stream through pump(), into a tar on the link that
 * unpacks it. 0, or -errno. */
static long remote_pipe(const char *src, const char *nm, const char *dst)
{
    char qs[4096 + 64], qn[4096 + 64], qd[4096 + 64], c1[4096 + 200], c2[4096 + 200];
    int a[2], b[2];
    long p1, p2, r = 0;
    int s1, s2;

    if (rquote(qs, sizeof qs, src) < 0 || rquote(qn, sizeof qn, nm) < 0 || rquote(qd, sizeof qd, dst) < 0)
        return -ENAMETOOLONG;
    strlcpy(c1, "tar -C ", sizeof c1);
    strlcat(c1, qs, sizeof c1);
    strlcat(c1, " -cf - -- ", sizeof c1);
    strlcat(c1, qn, sizeof c1);
    strlcpy(c2, "tar -C ", sizeof c2);
    strlcat(c2, qd, sizeof c2);
    strlcat(c2, " -xf -", sizeof c2);
    if (pipe2(a, O_CLOEXEC) < 0) return -EMFILE;
    if (pipe2(b, O_CLOEXEC) < 0) {
        close(a[0]);
        close(a[1]);
        return -EMFILE;
    }
    p1 = rssh_start(c1, -1, a[1]);
    p2 = rssh_start(c2, b[0], -1);
    close(a[1]);
    close(b[0]);
    r = p1 < 0 || p2 < 0 ? -EIO : pump(a[0], b[1]);
    close(a[0]);
    close(b[1]);
    s1 = p1 < 0 ? -1 : wait_pid(p1);
    s2 = p2 < 0 ? -1 : wait_pid(p2);
    return !r && (s1 != 0 || s2 != 0) ? -EIO : r;
}

/* Copies (or moves, if MOVE) the items of the link panel P into DST on the link: a
 * directory of them with its contents, and a single item to the name DST when it is not
 * a directory. An existing name is not replaced. 0, or -errno. */
static long remote_copy(const struct panel *p, const char *dst, int sel, int move, int isdir)
{
    char q[4096 + 64], cmd[4096 + 300], target[4352], from[4352];
    long r = 0;

    for (int d = 0; d < p->n && !r; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        const char *nm = p->nm + e->name;

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        if (isdir ? join(target, sizeof target, dst, nm) : str_fit(target, sizeof target, dst))
            return -ENAMETOOLONG;
        r = remote_clash(target);
        if (r) return r;
        if (move || !isdir) {                  /* a move, or a rename: one command on the link */
            if (join(from, sizeof from, p->path, nm)) return -ENAMETOOLONG;
            if (rquote(q, sizeof q, from) < 0) return -ENAMETOOLONG;
            strlcpy(cmd, move ? "mv -- " : "cp -a -- ", sizeof cmd);
            strlcat(cmd, q, sizeof cmd);
            if (rquote(q, sizeof q, target) < 0) return -ENAMETOOLONG;
            strlcat(cmd, " ", sizeof cmd);
            strlcat(cmd, q, sizeof cmd);
            strlcat(cmd, " 2>/dev/null && echo '### 200' || echo '### 500'", sizeof cmd);
            r = rrun(cmd, NULL, NULL) == 200 ? 0 : -EIO;
        } else {
            r = remote_pipe(p->path, nm, dst);
        }
    }
    return r;
}

/* The bytes of one line of find -printf '%y %s' on the link, as a scan counts them. */
static void rscan_line(void *x, const char *l)
{
    const char *q = l + 2;
    uint64_t n;

    (void)x;
    if (!l[0] || l[1] != ' ') return;
    n = (uint64_t)rnum(&q);
    if (l[0] == 'd') {
        if (pt_tarish) pt_total += 512;
    } else if (pt_tarish) {
        pt_total += 512 + ((n + 511) >> 9 << 9);
    } else {
        pt_total += n;
    }
}

/* The bytes of an item of the link, which may be a directory with all in it. */
static void remote_scan(const char *path)
{
    char q[4096 + 64], cmd[4096 + 200];

    if (rr < 0 || rquote(q, sizeof q, path) < 0) return;
    strlcpy(cmd, "find -H ", sizeof cmd);
    strlcat(cmd, q, sizeof cmd);
    strlcat(cmd, " -printf '%y %s\\n' 2>/dev/null; echo '### 200'", sizeof cmd);
    rrun(cmd, rscan_line, NULL);
}

/* The bytes of the items of P that a job works on, found under BASE (the panel's
 * directory, or the staging one). REMOTE scans the link, else this machine. */
static void scan_items(const struct panel *p, const char *base, int sel, int remote)
{
    for (int d = 0; d < p->n; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        char path[4352];

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        if (join(path, sizeof path, base, p->nm + e->name)) continue;
        if (remote) remote_scan(path);
        else scan_path(path);
    }
}
