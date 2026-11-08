/* ---- extraction: the members of an archive that are taken, written under a
 * directory of their own (the staging directory). A member is written only where
 * its parent directories are real directories: nothing is written through a
 * symbolic link, and a name with an empty, "." or ".." component is never written. */

/* What an extraction takes, and where it goes. The paths taken are packed in buf,
 * NUL-terminated, and off[] gives where each one is. */
struct xout {
    char     root[4096];        /* the staging directory, "" when there is none */
    uint32_t n;
    uint32_t off[MAXE];
    size_t   used;
    char     buf[NPOOL];
    long     err;               /* the first failure: 0 if none */
};
static struct xout xo;

static long xo_add(const char *name)
{
    size_t l = strlen(name);

    if (xo.n >= MAXE || xo.used + l + 1 > NPOOL) return -ENOSPC;
    for (size_t k = 0; k <= l; k++) xo.buf[xo.used + k] = name[k];
    xo.off[xo.n++] = (uint32_t)xo.used;
    xo.used += l + 1;
    return 0;
}

/* Whether NAME is one of the paths taken, or lies inside a directory taken. */
static int xwanted(const struct xout *x, const char *name)
{
    for (uint32_t i = 0; i < x->n; i++) {
        const char *s = x->buf + x->off[i];
        size_t l = strlen(s), k;

        for (k = 0; k < l && name[k] == s[k]; k++)
            ;
        if (k == l && (name[l] == 0 || name[l] == '/')) return 1;
    }
    return 0;
}

/* Whether NAME has an empty, "." or ".." component. */
static int unsafe_name(const char *name)
{
    size_t s = 0;

    for (size_t k = 0;; k++) {
        if (name[k] == '/' || name[k] == 0) {
            size_t l = k - s;

            if (l == 0 || (l == 1 && name[s] == '.') ||
                (l == 2 && name[s] == '.' && name[s + 1] == '.'))
                return 1;
            if (name[k] == 0) return 0;
            s = k + 1;
        }
    }
}

static void xfail(struct xout *x, long r)
{
    if (r && !x->err) x->err = r;
}

/* Whether PATH is a directory, and not a link to one. */
static int is_real_dir(const char *path)
{
    struct statx sx;

    return statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0 &&
           (sx.stx_mode & S_IFMT) == S_IFDIR;
}

/* Makes the directories above NAME under ROOT, one by one. A component that is not
 * a real directory (a file, or a link) stops it: -ENOTDIR. */
static long xparents(const char *root, const char *name)
{
    char path[4352];
    size_t pl, s = 0;

    strlcpy(path, root, sizeof path);
    pl = strlen(path);
    for (size_t k = 0; name[k]; k++) {
        long r;

        if (name[k] != '/') continue;
        path[pl++] = '/';
        for (size_t j = s; j < k; j++) path[pl++] = name[j];
        path[pl] = 0;
        r = mkdirat(AT_FDCWD, path, 0755);
        if (r < 0 && r != -EEXIST) return r;
        if (!is_real_dir(path)) return -ENOTDIR;
        s = k + 1;
    }
    return 0;
}

/* A member is written by FILL, which writes its bytes into the file FD, or, when
 * there is no file (FD < 0), only consumes them: a tar stream must move on. */
static long xmember(struct xout *x, const char *name, int mode,
                    long (*fill)(void *, long), void *arg)
{
    char path[4352];
    long fd = -ENAMETOOLONG, r;

    if (!join(path, sizeof path, x->root, name)) {
        r = xparents(x->root, name);
        fd = r ? r : open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, (mode_t)mode);
    }
    r = fill(arg, fd);
    if (fd >= 0) close((int)fd);
    else r = fd;
    if (r) {
        if (fd >= 0) unlinkat(AT_FDCWD, path, 0);
        xfail(x, r);
    }
    return r;
}

/* A hard link's content: a copy of the file it is linked to, already written. The
 * source is not followed through a link: O_NOFOLLOW on its last part. */
static long hard_fill(void *arg, long fd)
{
    const char *src = arg;
    uint8_t buf[4096];
    long in, r = fd < 0 ? fd : 0;

    if (fd < 0) return fd;
    in = open(src, O_RDONLY | O_NOFOLLOW, 0);
    if (in < 0) return in;
    for (;;) {
        long k = read((int)in, buf, sizeof buf);

        if (k == -EINTR) continue;
        if (k < 0) {
            r = k;
            break;
        }
        if (k == 0) break;
        if (!r) r = write_all(fd, buf, (size_t)k);
    }
    close((int)in);
    return r;
}

static void xdir(struct xout *x, const char *name)
{
    char path[4352];
    long r = join(path, sizeof path, x->root, name);

    if (!r) r = xparents(x->root, name);
    if (!r) r = mkdirat(AT_FDCWD, path, 0755);
    if (r == -EEXIST && is_real_dir(path)) r = 0;
    xfail(x, r);
}

static void xlink(struct xout *x, const char *name, const char *target)
{
    char path[4352];
    long r = join(path, sizeof path, x->root, name);

    if (!r) r = xparents(x->root, name);
    if (!r) r = symlinkat(target, AT_FDCWD, path);
    xfail(x, r);
}

static void xhard(struct xout *x, const char *name, const char *target)
{
    char src[4352];
    size_t n;
    int dir;
    const char *t = target ? norm_name(target, &n, &dir) : NULL;
    long r = !t || unsafe_name(t) ? -EINVAL : join(src, sizeof src, x->root, t);

    if (!r) r = xparents(x->root, t);
    if (r) {
        xfail(x, r);
        return;
    }
    xmember(x, name, 0644, hard_fill, src);
}

/* The temporary directory of an extraction: a new one, under TMPDIR, or /tmp. Its
 * path is xo.root. */
static long xtmp_make(void)
{
    const char *t = genv("TMPDIR");
    char num[32];
    long pid = getpid();

    if (!t || !*t) t = "/tmp";
    for (unsigned k = 0; k < 1000; k++) {
        long r;
        int l;

        strlcpy(xo.root, t, sizeof xo.root);
        strlcat(xo.root, "/mc-", sizeof xo.root);
        l = (int)utoa((unsigned long)pid, num);
        num[l] = 0;
        strlcat(xo.root, num, sizeof xo.root);
        strlcat(xo.root, "-", sizeof xo.root);
        l = (int)utoa(k, num);
        num[l] = 0;
        strlcat(xo.root, num, sizeof xo.root);
        r = mkdirat(AT_FDCWD, xo.root, 0700);
        if (r == 0) return 0;
        if (r != -EEXIST) break;
    }
    xo.root[0] = 0;
    return -EEXIST;
}
