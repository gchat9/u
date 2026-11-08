/* ------------------------------------------------------------------ archives
 *
 * An archive that is entered as a directory (see enter_item) is read once, into an
 * index of every entry in it: its name, size, date and kind. The directory shown is
 * listed from the index. The index is kept while the archive file is the one it was
 * read from (archive_fresh), so going in and out of directories reads nothing again.
 *
 * The kinds read: .zip from its central directory; .tar from the file itself; .tgz,
 * .tar.gz, .txz, .tar.xz, .tar.zst and .tar.zstd from the output of gzip, xz or zstd
 * (-dc) through a pipe. A compressed tar has no table of contents, so its headers are
 * read in one pass over the stream, and the data of each entry is skipped. Nothing is
 * written to disk.
 *
 * A file of an archive is written out only to be shown (F3) or copied (F5): into a
 * directory of its own under TMPDIR, which is removed afterwards. Nothing is ever
 * written into an archive, which is read-only. */

/* The kinds of entry, in an index. */
enum { AE_FILE, AE_DIR, AE_LINK, AE_HARD };

/* One entry of an archive, in its index. */
struct aent {
    uint32_t name;                      /* offset of its name in the index's names */
    uint32_t tgt;                       /* for a link: offset of its target, plus 1; else 0 */
    uint32_t size;                      /* size_pack() */
    uint32_t mtime;                     /* seconds since 1970 */
    uint32_t kind;                      /* AE_* */
};

/* An archive as it was read: its index, and the file it was read from. */
struct archive {
    size_t      map;                    /* bytes mapped, for munmap */
    uint64_t    size, ino;              /* the archive file, as it was read */
    int64_t     mtime, mnsec;
    uint32_t    n, nn;                  /* entries, and bytes of names, in use */
    struct aent *e;
    char        *nm;
};

/* A new, empty index. Its memory is anonymous, and it costs little until it fills. */
static struct archive *archive_new(void)
{
    size_t hdr = (sizeof(struct archive) + 15) & ~(size_t)15;
    size_t total = hdr + AR_MAXE * sizeof(struct aent) + AR_NPOOL;
    uint8_t *m = mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct archive *a;

    if (m == MAP_FAILED) return NULL;
    a = (struct archive *)m;
    a->map = total;
    a->e = (struct aent *)(m + hdr);
    a->nm = (char *)(m + hdr + AR_MAXE * sizeof(struct aent));
    return a;
}

static void archive_free(struct archive *a)
{
    if (a) munmap(a, a->map);
}

/* Leave the archive, if the panel is in one: its index is freed. */
static void vfs_forget(struct panel *p)
{
    archive_free(p->ar);
    p->ar = NULL;
    p->arc[0] = 0;
    p->sub[0] = 0;
}

/* The archive file of panel P, as a path. */
static void archive_path(char *full, size_t max, const struct panel *p)
{
    strlcpy(full, p->path, max);
    strlcat(full, "/", max);
    strlcat(full, p->arc, max);
}

/* A member's name, as it is kept: leading "/" and "./" dropped, and a trailing "/"
 * makes it a directory (*dir). Its length, without that "/", is *n. NULL if it cannot
 * be kept: empty, or longer than 255 bytes. */
static const char *norm_name(const char *name, size_t *n, int *dir)
{
    for (;;) {
        if (name[0] == '/') name++;
        else if (name[0] == '.' && name[1] == '/') name += 2;
        else break;
    }
    *n = strlen(name);
    *dir = 0;
    if (*n && name[*n - 1] == '/') {
        *dir = 1;
        (*n)--;
    }
    return *n == 0 || *n > 255 ? NULL : name;
}

/* Add an entry to the index A. NAME is its path inside the archive (see norm_name).
 * TARGET is a link's target, if it has one. -1 if the index is full. */
static int archive_add(struct archive *a, const char *name, uint64_t size, uint64_t mtime,
                       int kind, const char *target)
{
    size_t n, tn = 0, k;
    int dir, withtgt = 0;
    struct aent *e;

    name = norm_name(name, &n, &dir);
    if (!name) return 0;
    if (dir) kind = AE_DIR;
    if ((kind == AE_LINK || kind == AE_HARD) && target && strlen(target) <= 255) {
        withtgt = 1;
        tn = strlen(target);
    }
    if (a->n >= AR_MAXE || a->nn + n + 1 + (withtgt ? tn + 1 : 0) > AR_NPOOL) return -1;

    for (k = 0; k < n; k++) a->nm[a->nn + k] = name[k];
    a->nm[a->nn + n] = 0;
    e = &a->e[a->n++];
    e->name = a->nn;
    e->tgt = 0;
    a->nn += (uint32_t)n + 1;
    if (withtgt) {
        for (k = 0; k < tn; k++) a->nm[a->nn + k] = target[k];
        a->nm[a->nn + tn] = 0;
        e->tgt = a->nn + 1;
        a->nn += (uint32_t)tn + 1;
    }
    e->size = size_pack(size);
    e->mtime = (uint32_t)mtime;
    e->kind = (uint32_t)kind;
    return 0;
}

/* The entry NM of an archive, if it is inside the directory p->sub, is listed in the
 * panel: as a file, a link or a directory, once however many entries are in it. */
static void vfs_add(struct panel *p, const char *nm, uint32_t size, uint32_t mtime, int kind)
{
    size_t sl = strlen(p->sub), k, l;
    const char *rest, *slash;
    char c[256];
    struct ent *e;
    int at, isdir;

    for (k = 0; k < sl; k++)
        if (nm[k] != p->sub[k]) return;
    rest = nm + sl;
    if (!*rest) return;
    slash = rest;
    while (*slash && *slash != '/') slash++;
    l = (size_t)(slash - rest);
    if (l == 0 || l > 255) return;
    for (k = 0; k < l; k++) c[k] = rest[k];
    c[l] = 0;
    if (!scmp(c, ".") || !scmp(c, "..")) return;
    isdir = *slash == '/' || kind == AE_DIR;    /* more names follow it, or it is one itself */
    if (isdir) {
        for (int q = 1; q < p->ne; q++)
            if (p->e[q].cls == CL_DIR && !scmp(p->nm + p->e[q].name, c)) return;
    }
    if (p->ne >= MAXE) return;
    at = add_name(p, c);
    if (at < 0) return;
    e = &p->e[p->ne++];
    e->name = (uint32_t)at;
    e->up = 0;
    e->sel = 0;
    e->link = !isdir && kind == AE_LINK;
    e->mtime = mtime;
    if (isdir) {
        e->cls = CL_DIR;
        e->size = 0;
    } else {
        e->cls = classify(c, 0);
        e->size = size;
    }
}

/* Every entry of the archive is listed, as the directory p->sub of it. */
static void archive_list(struct panel *p)
{
    const struct archive *a = p->ar;

    for (uint32_t i = 0; a && i < a->n; i++)
        vfs_add(p, a->nm + a->e[i].name, a->e[i].size, a->e[i].mtime, (int)a->e[i].kind);
}

/* The target of the link NAME, in the directory p->sub of the archive shown, into TG.
 * 0 if there is none. */
static int vfs_target(const struct panel *p, const char *name, char *tg, size_t max)
{
    const struct archive *a = p->ar;
    char full[512];

    if (!a) return 0;
    strlcpy(full, p->sub, sizeof full);
    strlcat(full, name, sizeof full);
    for (uint32_t i = 0; i < a->n; i++) {
        const struct aent *x = &a->e[i];

        if (x->kind == AE_LINK && x->tgt && !scmp(a->nm + x->name, full)) {
            strlcpy(tg, a->nm + x->tgt - 1, max);
            return 1;
        }
    }
    return 0;
}
