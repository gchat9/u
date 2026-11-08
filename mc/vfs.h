/* Run TOOL on the file FULL, to decompress it with -dc: its output is a pipe, and its
 * read end is returned, with the process in *pid. A negative errno if it cannot be
 * started. Its input is /dev/null, and its errors go there too. */
static long ar_spawn(const char *full, const char *tool, long *pid)
{
    char *argv[4];
    int pf[2];
    long p, dn, r;

    argv[0] = (char *)tool;
    argv[1] = (char *)"-dc";
    argv[2] = (char *)full;
    argv[3] = NULL;
    r = pipe2(pf, O_CLOEXEC);
    if (r < 0) return r;
    p = fork_child();
    if (p < 0) {
        close(pf[0]);
        close(pf[1]);
        return p;
    }
    if (p == 0) {
        dn = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
        dup2(pf[1], 1);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 2);
        }
        exec_tool(tool, argv);
        exit(127);
    }
    close(pf[1]);
    *pid = p;
    return pf[0];
}

/* Wait for the decompressor PID. Unless the index filled (FULL), a failure of the
 * process is a failure of the archive. */
static int ar_wait(long pid, int full, int rc)
{
    int st = 0;

    while (wait4((int)pid, &st, 0, NULL) == -EINTR)
        ;
    if (!full && rc == 0 && ((st & 0x7f) || ((st >> 8) & 0xff))) return -EIO;
    return rc;
}

/* Reads the archive of panel P in one pass: indexed into A if A is given, else its
 * entries taken into X. 0, or a negative errno. */
static long archive_scan(const struct panel *p, struct archive *a, struct xout *x)
{
    char full[4096 + 260];
    struct statx sx;
    const char *tool;
    long fd, pid = 0, out;
    int kind = archive_kind(p->arc), rc = 0, full_idx = 0;

    archive_path(full, sizeof full, p);
    if (statx(AT_FDCWD, full, 0, STATX_BASIC_STATS, &sx) != 0 ||
        (sx.stx_mode & S_IFMT) != S_IFREG)      /* not a file: open() could block */
        return -EINVAL;
    fd = open(full, O_RDONLY, 0);
    if (fd < 0) return fd;
    /* The index is of this file, and the signature is taken from it. */
    if (statx((int)fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) != 0 ||
        (sx.stx_mode & S_IFMT) != S_IFREG) {
        close((int)fd);
        return -EINVAL;
    }
    if (a) {
        a->size = sx.stx_size;
        a->ino = sx.stx_ino;
        a->mtime = sx.stx_mtime.tv_sec;
        a->mnsec = sx.stx_mtime.tv_nsec;
    }

    switch (kind) {
    case AR_ZIP:
        rc = (int)zip_walk((int)fd, sx.stx_size, a ? zip_index_cb : zip_extract_cb,
                           a ? (void *)a : (void *)x);
        break;
    case AR_TAR:
        rc = tar_scan(a, fd, 0, &full_idx, x);
        break;
    case AR_GZ:
    case AR_XZ:
    case AR_ZST:
        tool = kind == AR_GZ ? "gzip" : kind == AR_XZ ? "xz" : "zstd";
        close((int)fd);
        fd = -1;
        out = ar_spawn(full, tool, &pid);
        if (out < 0) {
            rc = (int)out;
            break;
        }
        rc = tar_scan(a, out, pid, &full_idx, x);
        close((int)out);
        rc = ar_wait(pid, full_idx, rc);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    if (fd >= 0) close((int)fd);
    return rc;
}

/* Read the archive of panel P (P->arc, in P->path) into a new index, which replaces
 * the index there is. 0, or a negative errno: then the index there is kept. */
static int archive_read(struct panel *p)
{
    struct archive *a = archive_new();
    int rc;

    if (!a) return -EIO;
    rc = (int)archive_scan(p, a, NULL);
    if (rc) {
        archive_free(a);
        return rc;
    }
    archive_free(p->ar);
    p->ar = a;
    return 0;
}

/* The targets of the hard links taken are staged too: a hard link is written as a copy
 * of its target, which is then in the staging directory, but is not copied itself. */
static void vfs_hard_targets(const struct panel *p)
{
    const struct archive *a = p->ar;

    for (uint32_t i = 0; a && i < a->n; i++) {
        const struct aent *x = &a->e[i];
        char tg[256];
        const char *t;
        size_t n;
        int dir;

        if (x->kind != AE_HARD || !x->tgt || !xwanted(&xo, a->nm + x->name)) continue;
        strlcpy(tg, a->nm + x->tgt - 1, sizeof tg);
        t = norm_name(tg, &n, &dir);
        if (t) xo_add(t);
    }
}

/* Takes the items of the archive panel P that a job works on (the marked ones, if SEL,
 * else the highlighted one) into a new staging directory, and writes them there in one
 * pass over the archive. The job then copies them from there. 0, or a negative errno. */
static long vfs_stage(const struct panel *p, int sel)
{
    long r;

    xo.n = 0;
    xo.used = 0;
    xo.err = 0;
    r = xtmp_make();
    if (r) return r;
    for (int d = 0; d < p->n; d++) {
        const struct ent *e = &p->e[p->ord[d]];
        char inner[512];

        if (e->up || (sel ? !e->sel : d != p->cur)) continue;
        strlcpy(inner, p->sub, sizeof inner);
        strlcat(inner, p->nm + e->name, sizeof inner);
        r = xo_add(inner);
        if (r) break;
        if (is_dir(e)) xdir(&xo, inner);
    }
    /* a member that fails is missing from the staging directory: the copy says so */
    if (!r) {
        vfs_hard_targets(p);
        r = archive_scan(p, NULL, &xo);
    }
    return r;
}

/* Whether the index of panel P is of the archive file as it is now. */
static int archive_fresh(const struct panel *p)
{
    char full[4096 + 260];
    struct statx sx;

    if (!p->ar) return 0;
    archive_path(full, sizeof full, p);
    if (statx(AT_FDCWD, full, 0, STATX_BASIC_STATS, &sx) != 0) return 0;
    return sx.stx_size == p->ar->size && sx.stx_ino == p->ar->ino &&
           sx.stx_mtime.tv_sec == p->ar->mtime && sx.stx_mtime.tv_nsec == p->ar->mnsec;
}
