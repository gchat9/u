/* ----------------------------------------------------------- compare directories */

/* Command, Compare directories: a question asks for the method, and then the marks
 * of both panels are set to the files that differ, each panel against the other, as
 * mc sets them. A file is marked if the other panel has no file of that name, or one
 * of another size, and it is not marked if it is the older of two versions. By Quick
 * a file of the same size is marked if it is the newer; by Thorough, if its bytes
 * differ; Size only stops at the size. Directories are never marked, and the marks a
 * panel had go. Thorough reads the files, so both panels must be directories on this
 * machine: an archive or a shell link is refused. */

/* The methods, which are the question's first three buttons. */
enum { CM_QUICK, CM_SIZE, CM_THOROUGH };

/* Whether the files at PA and PB have the same bytes. Each is read half of ob at a
 * time, until both end. A file that cannot be opened is not the same. */
static int same_bytes(const char *pa, const char *pb)
{
    size_t h = sizeof ob / 2, na, nb, i;
    char *a = ob, *b = ob + h;
    long fa = open(pa, O_RDONLY, 0), fb = open(pb, O_RDONLY, 0);
    int same = fa >= 0 && fb >= 0;

    while (same) {
        na = read_full(fa, a, h);
        nb = read_full(fb, b, h);
        same = na == nb;
        for (i = 0; same && i < na; i++) same = a[i] == b[i];
        if (na < h) break;
    }
    if (fa >= 0) close((int)fa);
    if (fb >= 0) close((int)fb);
    return same;
}

/* Whether entry S of P and entry T of O, the same name in the other panel, have
 * different bytes. A path too long for the buffers is taken as different. */
static int bytes_differ(const struct panel *p, const struct ent *s, const struct panel *o,
                        const struct ent *t)
{
    char pa[4096], pb[4096];

    if (join(pa, sizeof pa, p->path, p->nm + s->name) ||
        join(pb, sizeof pb, o->path, o->nm + t->name))
        return 1;
    return !same_bytes(pa, pb);
}

/* Whether the exact sizes of entry S of P and entry T of O differ, where their sizes
 * round alike (size_pack): a size of a GiB or more is kept to the unit, so two of them
 * can be alike in the listing and not in fact. Only local files are stat'ed, as a
 * link's own size; an archive or a shell link keeps no exact size, and then the sizes
 * stay as alike as the listing has them. A file that cannot be stat'ed is different. */
static int exact_size_differs(const struct panel *p, const struct ent *s,
                              const struct panel *o, const struct ent *t)
{
    char pa[4096], pb[4096];
    struct statx sa, sb;

    if (p->arc[0] || p->rem || o->arc[0] || o->rem) return 0;
    if (join(pa, sizeof pa, p->path, p->nm + s->name) ||
        join(pb, sizeof pb, o->path, o->nm + t->name))
        return 1;
    if (statx(AT_FDCWD, pa, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sa) ||
        statx(AT_FDCWD, pb, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sb))
        return 1;
    return sa.stx_size != sb.stx_size;
}

/* The marks of P, set by method HOW against the panel O. */
static void cmp_panel(struct panel *p, const struct panel *o, int how)
{
    for (int i = 0; i < p->ne; i++) {
        struct ent *s = &p->e[i];
        const struct ent *t = NULL;
        int diff;

        s->sel = 0;
        if (s->up || (s->cls == CL_DIR && !s->link)) continue;
        for (int j = 0; j < o->ne && !t; j++)
            if (!scmp(p->nm + s->name, o->nm + o->e[j].name)) t = &o->e[j];
        if (!t) diff = 1;
        else if (how != CM_SIZE && s->mtime < t->mtime) diff = 0;
        else if (s->size != t->size) diff = 1;
        else if (s->size & 3 && exact_size_differs(p, s, o, t)) diff = 1;  /* a GiB or more */
        else if (how == CM_SIZE) diff = 0;
        else if (how == CM_QUICK) diff = s->mtime > t->mtime;
        else diff = bytes_differ(p, s, o, t);
        s->sel = (uint8_t)diff;
    }
}

/* The question's method HOW, chosen: the two panels are compared, each with the
 * other. Thorough needs the bytes of both, which an archive or a shell link does not
 * give here, so it is refused in them. */
static void cmp_run(int how)
{
    struct panel *a = &P[act], *b = &P[act ^ 1];
    const struct panel *x = a->arc[0] || a->rem ? a : b;

    if (how == CM_THOROUGH && (a->arc[0] || a->rem || b->arc[0] || b->rem)) {
        set_error("Thorough compare is not done in archives or links yet",
                  x->arc[0] ? x->arc : x->path, EINVAL);
        return;
    }
    cmp_panel(a, b, how);
    cmp_panel(b, a, how);
}

/* Command, Compare directories: the question of the method, Quick selected. */
static void cmp_open(void)
{
    qy_kind = QY_CMP;
    qy_n1 = cat(qy_t1, "Select compare method:");
    qy_n2 = 0;
    qy_sel = 0;
    mode = MODE_QUERY;
    md = 0;
}
