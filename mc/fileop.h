/* The job: F5, F6 or F8 done to a set of files. The set is the marked files and
 * directories of the panel if any is marked, else the highlighted item. Each item
 * is copied, moved or deleted in turn, and a directory is done as a whole: what is
 * in it is done too, recursively. A question is asked while the job runs, and the
 * job waits for its answer (modal_wait), so walking down a tree is plain recursion.
 * Esc, or Abort, stops the job at once: an item it has not finished keeps its mark. */
enum { JOB_COPY, JOB_MOVE, JOB_DEL };
static int job_cut;             /* the last job stopped before its items were all done */
static struct {
    int kind;                   /* JOB_COPY, JOB_MOVE or JOB_DEL */
    int sel;                    /* the marked items, not just the highlighted one */
    int dir;                    /* the destination is a directory: an item keeps its name in it */
    int pos;                    /* the next display index of the panel to look at */
    int skip_err;               /* Skip all was chosen: failures pass items over */
} job;
static char job_dst[4096];      /* the destination: a directory, or the file itself */
/* where the items are: the panel's directory, or the staging directory */
static char job_base[4096];
static long job_skips;          /* items passed over: a move keeps a source whose copy skipped any */

/* How a file is written, and what is decided about one that exists. */
enum { HOW_NEW, HOW_OVER, HOW_APPEND };
enum { DEC_NONE, DEC_NEW, DEC_OVER, DEC_APPEND, DEC_SKIP };

/* The overwrite question, as mc asks it. cf_group is the group answer that stands for
 * the rest of the job (CF_ASK: the question is asked for each file). The figures are
 * those of the pair in hand; cf_srcp and cf_dstp are its paths, as the question shows. */
enum { CF_ASK, CF_ALL, CF_OLDER, CF_NONE, CF_SMALLER, CF_SIZE };
#define CF_H 15                 /* rows of the question's box */
static int cf_group, cf_zero, cf_sel, cf_ans;
static uint64_t cf_ssize, cf_dsize;
static int64_t cf_smt, cf_dmt;
static const char *cf_srcp, *cf_dstp;

static int dgrp;                /* the answer to non-empty directories: 0 ask, 1 All, 2 None */
static int qy_ans;              /* the answer given to a failure or a directory question */

/* The progress of a job: the bytes done, out of the total the scans give, and the bytes done
 * before the file in hand (see pg_draw). */
static uint64_t pt_now, pt_total, pt_base;
static int pt_tarish;                    /* a scan counts what a tar stream spends on an item */
static const char *pg_src, *pg_dst;     /* the progress box: the source name, the destination */
static int pg_pct;                      /* percentage the progress box shows, -1 before the first */

/* The rule of the group answer for the pair in hand, as mc has it: a zero-length file
 * is not written over when the checkbox says so, except by Yes or Smaller. DEC_NONE
 * when the question is to be asked (CF_ASK). */
static int conflict_rule(void)
{
    int zero = cf_zero && cf_ssize == 0;

    switch (cf_group) {
    case CF_ALL:     return zero ? DEC_SKIP : DEC_OVER;
    case CF_OLDER:   return cf_smt > cf_dmt && !zero ? DEC_OVER : DEC_SKIP;
    case CF_NONE:    return DEC_SKIP;
    case CF_SMALLER: return cf_ssize > cf_dsize ? DEC_OVER : DEC_SKIP;
    case CF_SIZE:    return cf_ssize != cf_dsize && !zero ? DEC_OVER : DEC_SKIP;
    }
    return DEC_NONE;
}

/* A percentage of DONE out of TOTAL, at most 100 (0 while the total is not known). */
static int pct_of(uint64_t done, uint64_t total)
{
    uint64_t p;

    if (!total) return 0;
    p = udiv64(done * 100, total, NULL);     /* no 64-bit divide instruction on armhf or i386 */
    return p > 100 ? 100 : (int)p;
}

/* Whether Esc, or A (the Abort of the progress box), has been typed alone, without
 * waiting for more input. Other keys typed during a copy are dropped. */
static int esc_typed(void)
{
    struct pollfd pf;
    unsigned char k[8];
    long n;

    pf.fd = 0;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0) return 0;
    n = read(0, k, sizeof k);
    if (n == 1 && (k[0] == 'a' || k[0] == 'A')) return 1;
    return n >= 1 && k[0] == K_ESC && (n == 1 || k[1] == K_ESC);
}

/* The progress of a copy or move, as the box shows it: the file in hand, and the whole
 * job (the Total bar), out of the bytes the job has as scanned before it starts. A
 * transfer to or from a link counts the bytes that pass through mc (pump). The box is
 * drawn at most every PG_MS, and only the rows that changed are written. */
#define PG_MS 100
static int64_t pg_ms;                        /* when the box was last drawn, in ms */

/* Draws the progress box, unless it was drawn less than PG_MS ago: then the next call
 * draws it, or the end of the job. FORCE draws it at once. */
static void pg_draw(int force)
{
    struct timespec ts;
    int64_t t;

    clock_gettime(CLOCK_REALTIME, &ts);
    t = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!force && t >= pg_ms && t - pg_ms < PG_MS) return;
    pg_ms = t;
    check_size();
    draw();
}

/* A chunk of the file in hand is done: DONE of its TOTAL bytes. */
static void pg_update(uint64_t done, uint64_t total)
{
    pg_pct = total ? pct_of(done, total) : 100;
    pt_now = pt_base + done;
    pg_draw(0);
}

/* The bytes read from IN written to OUT, counted as the job's: the bytes of a transfer
 * through mc. 0 at the end of IN, or -errno; -ECANCELED if Esc is typed. */
static long pump(int in, int out)
{
    for (;;) {
        long n = read(in, ob, sizeof ob), off = 0;

        if (n == -EINTR) continue;
        if (n <= 0) return n;
        while (off < n) {
            long w = write(out, ob + off, (size_t)(n - off));

            if (w == -EINTR) continue;
            if (w <= 0) return w < 0 ? w : -EIO;
            off += w;
        }
        pt_now += (uint64_t)n;
        pg_pct = pct_of(pt_now, pt_total);
        pg_draw(0);
        if (esc_typed()) return -ECANCELED;
    }
}

/* The bytes of an item of a job, as a scan counts them: a file's size, or with
 * pt_tarish what a tar stream spends on it (a header, and its blocks); 512 for a
 * directory, which is then scanned in turn. */
struct scan_ctx { const char *dir; };
static long each_entry(const char *path, long (*fn)(const char *name, void *ctx), void *ctx);
static void scan_path(const char *path);

static long scan_child(const char *name, void *ctx)
{
    char c[4352];

    if (join(c, sizeof c, ((struct scan_ctx *)ctx)->dir, name) == 0) scan_path(c);
    return 0;
}

static void scan_path(const char *path)
{
    struct statx sx;
    struct scan_ctx ctx;

    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) != 0) return;
    if ((sx.stx_mode & S_IFMT) == S_IFDIR) {
        ctx.dir = path;
        if (pt_tarish) pt_total += 512;
        each_entry(path, scan_child, &ctx);
    } else if (pt_tarish) {
        pt_total += 512 + ((sx.stx_size + 511) >> 9 << 9);
    } else {
        pt_total += sx.stx_size;
    }
}

/* A job begins on the items of a source and a destination: the box shows it, with no
 * bytes done yet. The scans that follow give its total. */
static void job_begin(const char *src, const char *dst)
{
    pt_now = pt_base = pt_total = 0;
    pt_tarish = 0;
    pg_ms = 0;
    pg_pct = -1;
    pg_src = src;
    pg_dst = dst;
    mode = MODE_PROG;
}

/* Copy IN to OUT a chunk at a time, through ob: nothing else uses ob while the
 * copy runs, as draw() only runs once the chunk is written. Esc stops it, and
 * the result is -ECANCELED. */
static long copy_data(long in, long out, uint64_t total)
{
    uint64_t done = 0;

    for (;;) {
        long n = read((int)in, ob, sizeof ob), off = 0;

        if (n == -EINTR) continue;
        if (n <= 0) return n;
        while (off < n) {
            long w = write((int)out, ob + off, (size_t)(n - off));

            if (w == -EINTR) continue;
            if (w <= 0) return w < 0 ? w : -EIO;
            off += w;
        }
        done += (uint64_t)n;
        pg_update(done, total);
        if (esc_typed()) return -ECANCELED;
    }
}

/* Copy the file or link SRC to DST, made with the mode of SRC; a link is copied as
 * a link. HOW says what to do if DST exists: HOW_NEW (it must not), HOW_OVER
 * (truncate it), HOW_APPEND (add to its end). A copy that fails leaves no DST
 * behind, but only one made here: an existing file that was written over stays
 * as far as the copy got. Only files and links are copied. Returns 0, or -errno. */
static long copy_file(const char *src, const char *dst, int how)
{
    struct statx sx;
    char tg[4096];
    long in, out, r, c;
    uint32_t type;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx);
    if (r) return r;
    type = sx.stx_mode & S_IFMT;
    if (type == S_IFLNK) {
        r = readlinkat(AT_FDCWD, src, tg, sizeof tg - 1);
        if (r < 0) return r;
        tg[r] = 0;
        if (how != HOW_NEW) unlinkat(AT_FDCWD, dst, 0);    /* a link is replaced, not written in */
        return symlinkat(tg, AT_FDCWD, dst);
    }
    if (type != S_IFREG) return -EINVAL;
    in = open(src, O_RDONLY, 0);
    if (in < 0) return in;
    out = open(dst, O_WRONLY | O_CREAT |
               (how == HOW_NEW ? O_EXCL : how == HOW_OVER ? O_TRUNC : O_APPEND),
               sx.stx_mode & 07777);
    if (out < 0) {
        close((int)in);
        return out;
    }
    if (how != HOW_APPEND) fchmod((int)out, sx.stx_mode & 07777);   /* not left to the umask */
    r = copy_data(in, out, sx.stx_size);
    close((int)in);
    c = close((int)out);
    if (c < 0 && !r) r = c;                 /* a write that failed late (NFS, quota) */
    if (r && how == HOW_NEW) unlinkat(AT_FDCWD, dst, 0);
    return r;
}

/* Copy or move SRC to DST, with HOW as in copy_file(). A move is a rename, which
 * replaces DST; it cannot append, and across file systems it is a copy and then
 * the removal of SRC. A new DST must not exist. Returns 0, or -errno. */
static long op_file(const char *src, const char *dst, int how)
{
    struct statx sx;
    long r;

    if (how == HOW_NEW && statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0)
        return -EEXIST;
    if (job.kind == JOB_COPY) return copy_file(src, dst, how);
    if (how != HOW_APPEND) {
        r = renameat2(AT_FDCWD, src, AT_FDCWD, dst, 0);
        if (r != -EXDEV) return r;
    }
    r = copy_file(src, dst, how);
    if (r == 0) r = unlinkat(AT_FDCWD, src, 0);
    return r;
}

/* the walk is recursive: these call each other */
static long del_tree(const char *path, int approved);
static long copy_tree(const char *src, const char *dst);
static long move_tree(const char *src, const char *dst);

/* A failure: the box says what failed, and why, and offers Skip, Skip all, Retry and
 * Abort. Returns 0 to pass the item over, 1 to try it again, or -ECANCELED. */
static long fail_ask(const char *head, const char *path, long e)
{
    int k = 0;

    if (job.skip_err) {
        job_skips++;
        return 0;
    }
    qy_kind = QY_ERR;
    qy_n1 = cat(qy_t1, head);
    qy_t1[qy_n1++] = ' ';
    qy_t1[qy_n1++] = '"';
    while (path[k] && qy_n1 < (int)sizeof qy_t1 - 3) qy_t1[qy_n1++] = path[k++];
    qy_t1[qy_n1++] = '"';
    if (e == -ESAME) {
        qy_n2 = cat(qy_t2, "Same file as the source");
    } else if (e == -ESUB) {
        qy_n2 = cat(qy_t2, "Inside the directory itself");
    } else {
        qy_n2 = cat(qy_t2, errstr(-e));
        qy_t2[qy_n2++] = ' ';
        qy_t2[qy_n2++] = '(';
        qy_n2 += (int)utoa((unsigned long)-e, qy_t2 + qy_n2);
        qy_t2[qy_n2++] = ')';
    }
    qy_sel = 0;
    mode = MODE_QUERY;
    modal_wait();
    if (kbuf_eof) return -ECANCELED;
    if (qy_ans == 1) job.skip_err = 1;
    if (qy_ans == 0 || qy_ans == 1) {
        job_skips++;
        return 0;
    }
    if (qy_ans == 2) return 1;
    return -ECANCELED;
}

/* An item that cannot be tried again: the box, and its answer. Retry counts as Skip. */
static long item_fail(const char *head, const char *path, long e)
{
    long a = fail_ask(head, path, e);

    if (a < 0) return a;
    if (a == 1) job_skips++;
    return 0;
}

/* The question for a destination that exists: 0 to write it as *how says, 1 to pass
 * the file over, -ESAME for the same file, or -ECANCELED. */
static long conflict(const char *src, const char *dst, int *how)
{
    struct statx ss, ds;
    int dec;

    *how = HOW_NEW;
    if (statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds)) return 0;
    if (statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss)) return 0;
    if (same_file(&ss, &ds)) return -ESAME;
    cf_ssize = ss.stx_size;
    cf_smt = ss.stx_mtime.tv_sec;
    cf_dsize = ds.stx_size;
    cf_dmt = ds.stx_mtime.tv_sec;
    cf_srcp = src;
    cf_dstp = dst;
    if (cf_group == CF_ASK) {
        cf_ans = -1;
        cf_sel = 0;
        mode = MODE_CONFLICT;
        modal_wait();
        if (kbuf_eof || cf_ans < 0 || cf_ans == 9) return -ECANCELED;
        switch (cf_ans) {
        case 0: dec = DEC_OVER; break;
        case 1: dec = DEC_SKIP; break;
        case 2: dec = DEC_APPEND; break;
        default:                            /* a group answer stands for the rest */
            cf_group = cf_ans == 4 ? CF_ALL : cf_ans == 5 ? CF_OLDER : cf_ans == 6 ? CF_NONE
                     : cf_ans == 7 ? CF_SMALLER : CF_SIZE;
            dec = conflict_rule();
        }
    } else {
        dec = conflict_rule();
    }
    if (dec == DEC_SKIP) {
        job_skips++;
        return 1;
    }
    *how = dec == DEC_OVER ? HOW_OVER : dec == DEC_APPEND ? HOW_APPEND : HOW_NEW;
    return 0;
}

/* A non-empty directory at the top of the job: is it deleted with what is in it? The
 * answer stands for the directories after it, as All and None say. Returns 1 to
 * delete it, 0 to skip it, or -ECANCELED. */
static long dir_ask(const char *path)
{
    int k = 0;

    if (dgrp) return dgrp == 1;
    qy_kind = QY_DIR;
    qy_n1 = cat(qy_t1, "Directory \"");
    while (path[k] && qy_n1 < (int)sizeof qy_t1 - 16) qy_t1[qy_n1++] = path[k++];
    qy_n1 += cat(qy_t1 + qy_n1, "\" not empty.");
    qy_n2 = cat(qy_t2, "Delete it recursively?");
    qy_sel = 0;
    mode = MODE_QUERY;
    modal_wait();
    if (kbuf_eof) return -ECANCELED;
    switch (qy_ans) {
    case 0: return 1;
    case 1: return 0;
    case 2: dgrp = 1; return 1;
    case 3: dgrp = 2; return 0;
    default: return -ECANCELED;
    }
}

/* Calls FN with each name in the directory PATH, but "." and "..", until FN returns
 * non-zero, which is what this returns; or the -errno of the directory, if it cannot
 * be read. */
static long each_entry(const char *path, long (*fn)(const char *name, void *ctx), void *ctx)
{
    long buf[128];                      /* 1 KiB, aligned for the records */
    long fd, n = 0, off, r = 0;

    fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return fd;
    while (!r && (n = getdents64((int)fd, buf, sizeof buf)) > 0) {
        for (off = 0; off < n && !r; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + off);
            const char *nm = d->d_name;

            off += d->d_reclen;
            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
            r = fn(nm, ctx);
        }
    }
    if (n < 0 && !r) r = n;
    close((int)fd);
    return r;
}

/* The directory's entries, one at a time, deleted. */
static long del_child(const char *name, void *ctx)
{
    const char *dir = ctx;
    char child[4352];
    long r = join(child, sizeof child, dir, name);

    if (r) return item_fail("Cannot remove", dir, r);
    return del_tree(child, 1);
}

/* Deletes PATH: a file or a link is unlinked; a directory is removed when empty.
 * When it is not, it is deleted with what is in it, if that is approved: a top item
 * is asked about (dir_ask), and the items under it are approved with it. Returns 0,
 * or -ECANCELED. */
static long del_tree(const char *path, int approved)
{
    struct statx sx;
    long r, a;

    for (;;) {
        if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx)) return 0;
        if ((sx.stx_mode & S_IFMT) != S_IFDIR) {
            r = unlinkat(AT_FDCWD, path, 0);
            if (r == 0) return 0;
            a = fail_ask("Cannot remove file", path, r);
        } else {
            r = unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
            if (r == 0) return 0;
            if (r == -ENOTEMPTY || r == -EEXIST) {
                if (!approved) {
                    a = dir_ask(path);
                    if (a <= 0) {
                        if (a == 0) job_skips++;
                        return a;
                    }
                    approved = 1;
                }
                r = each_entry(path, del_child, (void *)path);
                if (r == -ECANCELED) return r;
                if (r == 0) {
                    r = unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
                    if (r == 0) return 0;
                }
            }
            a = fail_ask("Cannot remove directory", path, r);
        }
        if (a < 0) return a;
        if (a == 0) return 0;
    }
}

struct tctx { const char *src, *dst; };

/* One entry of a directory being copied or moved: its source and destination. */
static long copy_child(const char *name, void *ctx)
{
    const struct tctx *t = ctx;
    char cs[4352], cd[4352];
    long r = join(cs, sizeof cs, t->src, name);

    if (!r) r = join(cd, sizeof cd, t->dst, name);
    if (r) return item_fail("Cannot copy to", t->dst, r);
    return copy_tree(cs, cd);
}

static long move_child(const char *name, void *ctx)
{
    const struct tctx *t = ctx;
    char cs[4352], cd[4352];
    long r = join(cs, sizeof cs, t->src, name);

    if (!r) r = join(cd, sizeof cd, t->dst, name);
    if (r) return item_fail("Cannot move to", t->dst, r);
    return move_tree(cs, cd);
}

/* A directory is made at DST with MODE: the umask has been at it, so the mode is set
 * after. Returns 0, 1 to pass the directory over, or -ECANCELED. */
static long make_dir(const char *dst, int mode)
{
    long r, a, fd;

    for (;;) {
        r = mkdirat(AT_FDCWD, dst, 0777);
        if (r == 0) break;
        if (r == -EEXIST) return 0;
        a = fail_ask("Cannot create directory", dst, r);
        if (a < 0) return a;
        if (a == 0) return 1;
    }
    fd = open(dst, O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        fchmod((int)fd, mode);
        close((int)fd);
    }
    return 0;
}

/* One file, link or directory-less item of a copy or move, as the question has it:
 * the destination is asked about when it exists, and a failure of the write is asked
 * about. Returns 0, or -ECANCELED. */
static long leaf_do(const char *src, const char *dst, int move)
{
    int how;
    long r, a;

    r = conflict(src, dst, &how);
    if (r == -ESAME) return item_fail(move ? "Cannot move to" : "Cannot copy to", dst, r);
    if (r == 1) return 0;
    if (r) return r;
    for (;;) {
        pg_src = src;
        pg_dst = dst;
        pg_pct = -1;
        mode = MODE_PROG;
        r = move ? op_file(src, dst, how) : copy_file(src, dst, how);
        if (r == 0 || r == -ECANCELED) return r;
        a = fail_ask(move ? "Cannot move to" : "Cannot copy to", dst, r);
        if (a < 0) return a;
        if (a == 0) return 0;
    }
}

/* One file of a job, with its bytes counted: the Total bar moves on past it. */
static long leaf(const char *src, const char *dst, int move)
{
    struct statx sx;
    uint64_t sz = 0;
    long r;

    if (statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0 &&
        (sx.stx_mode & S_IFMT) != S_IFDIR)
        sz = sx.stx_size;
    pt_base = pt_now;
    r = leaf_do(src, dst, move);
    pt_now = pt_base + sz;
    return r;
}

/* A copy of SRC to DST: a file, a link, or a directory made, and what is in it copied
 * too; a directory that is there already is merged into. Returns 0, or -ECANCELED. */
static long copy_tree(const char *src, const char *dst)
{
    struct statx ss, ds;
    struct tctx t;
    long r;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss);
    if (r) return item_fail("Cannot copy", src, r);
    if ((ss.stx_mode & S_IFMT) != S_IFDIR) return leaf(src, dst, 0);
    if (under(dst, src)) return item_fail("Cannot copy to", dst, -ESUB);
    r = statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds);
    if (r == 0) {
        if ((ds.stx_mode & S_IFMT) != S_IFDIR) return item_fail("Cannot copy to", dst, -ENOTDIR);
        if (same_file(&ss, &ds)) return item_fail("Cannot copy to", dst, -ESAME);
    } else {
        r = make_dir(dst, ss.stx_mode & 07777);
        if (r) return r < 0 ? r : 0;
    }
    t.src = src;
    t.dst = dst;
    r = each_entry(src, copy_child, &t);
    if (r == -ECANCELED) return r;
    if (r) return item_fail("Cannot copy", src, r);
    return 0;
}

/* A move of SRC to DST: a rename when DST is free; across file systems, a copy and
 * then the removal of SRC, which is kept if anything in it was passed over. Into a
 * directory that is there, the move is into it, item by item, and the directory
 * goes when it is empty. Returns 0, or -ECANCELED. */
static long move_tree(const char *src, const char *dst)
{
    struct statx ss, ds;
    struct tctx t;
    long r, s0;

    r = statx(AT_FDCWD, src, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ss);
    if (r) return item_fail("Cannot move", src, r);
    if ((ss.stx_mode & S_IFMT) != S_IFDIR) return leaf(src, dst, 1);
    if (under(dst, src)) return item_fail("Cannot move to", dst, -ESUB);
    r = statx(AT_FDCWD, dst, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &ds);
    if (r == -ENOENT) {
        r = renameat2(AT_FDCWD, src, AT_FDCWD, dst, 0);
        if (r == 0) return 0;
        if (r != -EXDEV) return item_fail("Cannot move to", dst, r);
        s0 = job_skips;
        r = copy_tree(src, dst);
        if (r) return r;
        if (job_skips != s0) return 0;
        return del_tree(src, 1);
    }
    if (r) return item_fail("Cannot move to", dst, r);
    if ((ds.stx_mode & S_IFMT) != S_IFDIR) return item_fail("Cannot move to", dst, -ENOTDIR);
    if (same_file(&ss, &ds)) return item_fail("Cannot move to", dst, -ESAME);
    t.src = src;
    t.dst = dst;
    s0 = job_skips;
    r = each_entry(src, move_child, &t);
    if (r == -ECANCELED) return r;
    if (r) return item_fail("Cannot move", src, r);
    if (job_skips != s0) return 0;
    r = unlinkat(AT_FDCWD, src, AT_REMOVEDIR);
    if (r == 0) return 0;
    return item_fail("Cannot remove directory", src, r);
}
