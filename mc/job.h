/* The display index of the next item the job works on, or -1 when there is none. */
static int job_next(const struct panel *p)
{
    for (; job.pos < p->n; job.pos++) {
        const struct ent *e = &p->e[p->ord[job.pos]];

        if (e->up) continue;
        if (job.sel ? e->sel : job.pos == p->cur) return job.pos++;
    }
    return -1;
}

/* One item of the job, at the top: SRC, and DST when copying or moving. */
static long job_item(const char *src, const char *dst)
{
    if (job.kind == JOB_DEL) {
        long r;

        if (!P[act].rem) return del_tree(src, 0);
        r = remote_rm(src);
        return r ? item_fail("Cannot delete", src, r) : 0;
    }
    if (job.kind == JOB_COPY) return copy_tree(src, dst);
    return move_tree(src, dst);
}

static void job_end(void)
{
    mode = MODE_NORMAL;
    rescan_changed();
}

/* The items, in display order, one after another. An item that Esc stopped keeps its
 * mark; every other item loses it, done or passed over. */
static void job_run(void)
{
    struct panel *p = &P[act];
    char src[4352], dst[4352];

    job.pos = 0;
    job.skip_err = 0;
    job_cut = 0;
    dgrp = 0;
    cf_group = CF_ASK;
    cf_zero = 0;
    cf_sel = 0;
    while (!kbuf_eof) {
        int at = job_next(p);
        const struct ent *e;
        long r;

        if (at < 0) break;
        e = &p->e[p->ord[at]];
        r = join(src, sizeof src, job_base, p->nm + e->name);
        if (!r) {
            if (job.dir) r = join(dst, sizeof dst, job_dst, p->nm + e->name);
            else r = strlcpy(dst, job_dst, sizeof dst) < sizeof dst ? 0 : -ENAMETOOLONG;
        }
        if (r) {
            if (item_fail("Cannot copy to", job_dst, r) == -ECANCELED) {
                job_cut = 1;
                break;
            }
            p->e[p->ord[at]].sel = 0;
            continue;
        }
        r = job_item(src, dst);
        if (r == -ECANCELED) {
            job_cut = 1;
            break;
        }
        p->e[p->ord[at]].sel = 0;
    }
    if (kbuf_eof) job_cut = 1;
    job_end();
}

/* The job on a link, with the items of P and the destination JOB_DST, which is a
 * directory if job.dir: a download (the items are staged, and the local job copies
 * them from there), an upload into the link, or a copy on the link. A move removes the
 * originals once they are all copied. A failure is an error box; Esc is not. */
static void remote_job(struct panel *p, const struct panel *d)
{
    int kind = job.kind, isdir = job.dir;
    long skips0 = job_skips, r = 0;

    job_begin(p->path, job_dst);
    if (p->rem && !d->rem) {
        pt_tarish = 1;
        scan_items(p, p->path, job.sel, 1);
        pg_draw(1);
        r = remote_stage(p, job.sel);
        if (!r) {
            pt_tarish = 0;
            scan_items(p, xo.root, job.sel, 0);
            pg_draw(1);
            strlcpy(job_base, xo.root, sizeof job_base);
            job.kind = JOB_COPY;
            job_run();
            job.kind = kind;
            if (kind == JOB_MOVE && !job_cut && job_skips == skips0)
                for (int i = 0; i < p->n && !r; i++) {
                    const struct ent *e = &p->e[p->ord[i]];
                    char from[4352];

                    if (e->up || (job.sel ? !e->sel : i != p->cur)) continue;
                    if (join(from, sizeof from, p->path, p->nm + e->name)) r = -ENAMETOOLONG;
                    else r = remote_rm(from);
                }
        }
    } else if (!p->rem && d->rem) {
        if (!isdir) {
            r = -ENOTDIR;
        } else {
            pt_tarish = 1;
            scan_items(p, p->path, job.sel, 0);
            pg_draw(1);
            r = local_upload(p, job_dst, job.sel, kind == JOB_MOVE);
        }
    } else {
        if (kind != JOB_MOVE && isdir) {
            pt_tarish = 1;
            scan_items(p, p->path, job.sel, 1);
            pg_draw(1);
        }
        r = remote_copy(p, job_dst, job.sel, kind == JOB_MOVE, isdir);
    }
    if (xo.root[0]) del_tree(xo.root, 1);        /* the staging directory of a download or an upload */
    xo.root[0] = 0;
    mode = MODE_NORMAL;
    if (r && r != -ECANCELED) set_error(kind == JOB_COPY ? "Cannot copy to" : "Cannot move to", job_dst, -r);
    panel_rescan(p);
    if (d->rem || p->rem) panel_rescan(&P[act ^ 1]);
}
