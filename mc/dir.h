/* dir.h - reading a panel: a directory, an archive or a shell link, listed and
 * sorted; and reading it again when something may have changed it. */

/* The key of an entry for the sort: its atime, ctime or inode, as statx gives it. */
static int64_t sort_key(int kf, const struct statx *sx)
{
    return kf == SF_ATIME ? sx->stx_atime.tv_sec : kf == SF_CTIME ? sx->stx_ctime.tv_sec
                                                                   : (int64_t)sx->stx_ino;
}

/* Read the directory of P->path into its list, sorted; KEY, if not NULL, takes
 * the key of each entry (see sort_key()). */
static int read_dir(struct panel *p, int64_t *key)
{
    long buf[1024];                 /* 8 KiB, aligned for dirent64 records */
    struct statx sx;
    struct linux_dirent64 *d = NULL;
    struct ent *e;
    long fd, r, off;
    int kf = p->sort & SO_FIELD;

    fd = open(p->path, O_RDONLY | O_DIRECTORY, 0);

    load_up(p);

    while (fd >= 0 && (r = getdents64((int)fd, buf, sizeof buf)) > 0) {
        for (off = 0; off < r; off += d->d_reclen) {
            d = (struct linux_dirent64 *)((char *)buf + off);
            const char *nm = d->d_name;
            int at;

            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
                continue;
            if (p->ne >= MAXE) continue;
            at = add_name(p, nm);
            if (at < 0) continue;

            e = &p->e[p->ne];
            e->name = (uint32_t)at;
            e->up = 0;
            e->sel = 0;
            e->size = 0;
            e->mtime = 0;
            e->cls = CL_NORMAL;
            e->link = 0;
            if (key) key[p->ne] = 0;
            if (statx((int)fd, nm, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) == 0) {
                if (key) key[p->ne] = sort_key(kf, &sx);
                uint32_t type = sx.stx_mode & S_IFMT;
                e->size = size_pack(sx.stx_size);      /* a link's own size and date */
                e->mtime = (uint32_t)sx.stx_mtime.tv_sec;
                if (type == S_IFLNK) {
                    struct statx tx;

                    e->link = 1;
                    if (statx((int)fd, nm, 0, STATX_BASIC_STATS, &tx) != 0)
                        e->cls = CL_STALE;              /* dangling */
                    else if ((tx.stx_mode & S_IFMT) == S_IFDIR)
                        e->cls = CL_DIR;                /* sorts with directories */
                } else if (type == S_IFDIR) {
                    e->cls = CL_DIR;
                } else {
                    e->cls = classify(nm, type == S_IFREG && (sx.stx_mode & 0111));
                }
            }
            p->ne++;
        }
    }
    if (fd >= 0) close((int)fd);

    sort_list(p, key);

    {
        struct statfs f;
        p->free_kb = p->total_kb = 0;
        if (statfs(p->path, &f) == 0 && f.f_bsize > 0) {
            p->free_kb  = ((uint64_t)(uid ? f.f_bavail : f.f_bfree) * (uint64_t)f.f_bsize) >> 10;
            p->total_kb = ((uint64_t)f.f_blocks * (uint64_t)f.f_bsize) >> 10;
        }
    }
    return 0;
}

/* The key buffer of a sort by atime, ctime or inode, one entry of the directory
 * after another: it is on the stack of this read, and gone when it returns. */
static int load_keyed(struct panel *p)
{
    int64_t key[MAXE];

    return read_dir(p, key);
}

/* Read the panel's directory again: p->path, or the archive p->arc at the
 * directory p->sub. 0 on success; a negative errno if an archive cannot be read,
 * when the listing is only "...". */
static int load(struct panel *p)
{
    int kf = p->sort & SO_FIELD;

    if (p->rem) return rload(p);

    if (p->arc[0]) {
        int rc = 0;

        load_up(p);
        if (!archive_fresh(p)) rc = archive_read(p);
        if (rc == 0) archive_list(p);
        sort_list(p, NULL);
        p->free_kb = p->total_kb = 0;               /* as mc: an archive has no free space shown */
        return rc;
    }
    if (kf == SF_ATIME || kf == SF_CTIME || kf == SF_INODE) return load_keyed(p);
    return read_dir(p, NULL);
}

/* Read the directory of P again. The highlight stays on the same name if it is
 * still there, else on the same position; the marks stay on the names that are
 * still there too. The names of the marked entries wait in ob, which is free
 * once what is in it is written (a command's prompt may be, when a program has run),
 * so a marked name past the end of ob is not kept. */
static void panel_rescan(struct panel *p)
{
    char name[256];
    int at = p->cur, i, k = 0, j;

    flush();                                    /* ob is needed, below */
    name[0] = 0;
    if (p->n > 0) strlcpy(name, p->nm + p->e[p->ord[p->cur]].name, sizeof name);
    for (i = 0; i < p->n; i++) {
        const struct ent *e = &p->e[p->ord[i]];
        size_t l = strlen(p->nm + e->name) + 1;

        if (e->sel && !e->up && (size_t)k + l <= sizeof ob) {
            memcpy(ob + k, p->nm + e->name, l);
            k += (int)l;
        }
    }
    if (load(p) != 0 && p->arc[0]) {            /* the archive is gone: its directory */
        vfs_forget(p);
        load(p);
    }
    for (j = 1; j < p->ne && k; j++) {
        const char *en = p->nm + p->e[j].name;
        for (int s = 0; s < k; s += (int)strlen(ob + s) + 1) {
            size_t q = 0;

            while (ob[s + (int)q] && ob[s + (int)q] == en[q]) q++;
            if (!ob[s + (int)q] && !en[q]) {
                p->e[j].sel = 1;
                break;
            }
        }
    }
    p->cur = at < p->n ? at : p->n - 1;
    if (p->cur < 0) p->cur = 0;
    for (i = 0; i < p->n; i++) {
        const char *en = p->nm + p->e[p->ord[i]].name;
        size_t k2 = 0;

        while (name[k2] && name[k2] == en[k2]) k2++;
        if (name[k2] == 0 && en[k2] == 0) {
            p->cur = i;
            break;
        }
    }
    scroll(p);
}

/* After something may have changed the files (a command, a viewer, the shell, a
 * file operation), both panels are read again: a file can change in place, its
 * size and date, without its directory changing at all. The highlight and the
 * marks stay on their names. */
static void rescan_changed(void)
{
    panel_rescan(&P[0]);
    panel_rescan(&P[1]);
}
