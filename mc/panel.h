/* panel.h - a panel: what an entry is (classified by its name), how the panel is
 * titled, the cursor in its list, the marks. */

/* The mark before a name: / directory, * executable, ~ link to a directory,
 * @ other link, ! dangling link, or a space. */
static char ent_mark(const struct ent *e)
{
    if (e->link) return e->cls == CL_STALE ? '!' : e->cls == CL_DIR ? '~' : '@';
    return e->cls == CL_DIR ? '/' : e->cls == CL_EXE ? '*' : ' ';
}

/* Path as shown in mc: $HOME becomes ~. Returns the length written. */
static int disp_path(char *out, int max, const char *path)
{
    const char *home = genv("HOME");
    size_t hl = home ? strlen(home) : 0;
    int n = 0;

    if (hl > 1 && strlen(path) >= hl) {
        size_t i = 0;
        while (i < hl && home[i] == path[i]) i++;
        if (i == hl && (path[hl] == 0 || path[hl] == '/')) {
            out[n++] = '~';
            path += hl;
        }
    }
    while (*path && n < max - 1) out[n++] = *path++;
    out[n] = 0;
    return n;
}

/* The kind of archive NAME is, by its name, in any case: AR_NONE if it is not one. */
static int archive_kind(const char *name)
{
    static const struct { const char *ext; uint8_t kind; } ar[] = {
        { ".zip", AR_ZIP },
        { ".tar", AR_TAR },
        { ".tgz", AR_GZ },    { ".tar.gz", AR_GZ },
        { ".txz", AR_XZ },    { ".tar.xz", AR_XZ },
        { ".tar.zst", AR_ZST }, { ".tar.zstd", AR_ZST },
    };
    size_t n = strlen(name);

    for (size_t i = 0; i < sizeof ar / sizeof ar[0]; i++) {
        size_t l = strlen(ar[i].ext), k;

        if (n <= l) continue;
        for (k = 0; k < l && lc(name[n - l + k]) == ar[i].ext[k]; k++)
            ;
        if (k == l) return ar[i].kind;
    }
    return AR_NONE;
}

/* The path at the top of a panel, as drawn. In an archive it is the archive's name
 * and the directory inside it, as mc's uzip shows: "archive.zip/uzip://a/b". */
static int panel_title(char *out, int max, const struct panel *p)
{
    if (p->rem) {
        strlcpy(out, "sh://", (size_t)max);
        strlcat(out, rhost, (size_t)max);
        strlcat(out, p->path, (size_t)max);
        return (int)strlen(out);
    }
    if (!p->arc[0]) return disp_path(out, max, p->path);
    strlcpy(out, p->arc, (size_t)max);
    strlcat(out, archive_kind(p->arc) == AR_ZIP ? "/uzip://" : "/utar://", (size_t)max);
    if (p->sub[0]) {
        strlcat(out, p->sub, (size_t)max);
        out[strlen(out) - 1] = 0;               /* no trailing / */
    }
    return (int)strlen(out);
}

/* Is EXT (no dot) one of the ';'-separated words in LIST? Case-insensitive. */
static int ext_in(const char *list, const char *ext)
{
    for (const char *s = list; *s; ) {
        const char *t = s;
        size_t n = 0;
        int ok = 1;

        while (t[n] && t[n] != ';') n++;
        if (n == 0 || ext[n] != 0) ok = 0;
        for (size_t i = 0; ok && i < n; i++)
            if (lc(s[i]) != lc(ext[i])) ok = 0;
        if (ok) return 1;
        s = t + n;
        if (*s == ';') s++;
    }
    return 0;
}

/* mc's file class for a non-directory: executable, core dump, temp file,
 * then the extension groups, in filehighlight.ini order. */
static uint8_t classify(const char *name, int exe)
{
    const char *dot = NULL, *ext;
    size_t len = strlen(name);

    if (exe) return CL_EXE;
    for (const char *s = name; *s; s++)
        if (*s == '.') dot = s;
    ext = dot ? dot + 1 : "";

    if (name[0] == 'c' && name[1] == 'o' && name[2] == 'r' && name[3] == 'e') {
        const char *r = name + 4;                 /* core, core.1234 */
        while (*r == '.' || (*r >= '0' && *r <= '9')) r++;
        if (!*r) return CL_CORE;
    }
    if (name[0] == '#' || (len && name[len - 1] == '~') ||
        ext_in("bak;part;tmp;$$$", ext))
        return CL_TEMP;
    for (size_t i = 0; i < sizeof cl_ext / sizeof cl_ext[0]; i++)
        if (ext_in(cl_ext[i].ext, ext)) return cl_ext[i].cls;
    return CL_NORMAL;
}

/* Append NAME to the name pool of P: its offset, or -1 if the pool is full. */
static int add_name(struct panel *p, const char *name)
{
    int len = (int)strlen(name) + 1, off = p->nn;

    if (off + len > NPOOL) return -1;
    for (int i = 0; i < len; i++) p->nm[off + i] = name[i];
    p->nn += len;
    return off;
}

/* The "..." entry, always first: its name is at offset 0 of the pool, and its date
 * is the mtime of the parent of the path as mc knows it (lexically, so through a
 * symlink it is not the real parent). Inside an archive, that is the directory the
 * archive is in. */
static void load_up(struct panel *p)
{
    struct ent *e = &p->e[0];
    struct statx sx;

    p->nn = 0;
    add_name(p, "..");
    e->name = 0;
    e->size = 0;
    e->cls = CL_DIR;
    e->up = 1;
    e->sel = 0;
    e->link = 0;
    e->mtime = 0;
    {
        char up[4096], path[4096 + 4];

        strlcpy(path, p->path, sizeof path);
        strlcat(path, "/..", sizeof path);
        canon(up, sizeof up, path);
        if (statx(AT_FDCWD, up, 0, STATX_BASIC_STATS, &sx) == 0)
            e->mtime = (uint32_t)sx.stx_mtime.tv_sec;
    }
    p->ne = 1;
}

/* A path that names a shell link, sh://... */
static int is_link(const char *s) { return s[0] == 's' && s[1] == 'h' && s[2] == ':' && s[3] == '/' && s[4] == '/'; }

/* Keep the cursor visible. As in mc, crossing the bottom edge moves the list
 * up by half a screen, and crossing the top edge moves it down by half a
 * screen; the top never goes past the last full screen. */
static void scroll(struct panel *p)
{
    int body = rows - 6, half;

    if (body < 1) body = 1;
    half = body / 2 > 0 ? body / 2 : 1;
    if (p->cur >= p->n) p->cur = p->n ? p->n - 1 : 0;
    while (p->cur >= p->top + body) p->top += half;
    if (p->top > p->n - body) p->top = p->n - body > 0 ? p->n - body : 0;
    while (p->cur < p->top && p->top > 0) {
        p->top -= half;
        if (p->top < 0) p->top = 0;
    }
}

/* The terminal size, if it has changed: the screen, the panels and the shell's
 * pseudo-terminal follow it. */
static void check_size(void)
{
    struct winsz ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.col && ws.row &&
        (ws.col != cols || ws.row != rows)) {
        cols = ws.col > MAXW ? MAXW : ws.col;
        rows = ws.row;
        if (sh_fd >= 0) size_pty(sh_fd);
        scroll(&P[0]);
        scroll(&P[1]);
        rfull = 1;
        if (!hidden) outs("\x1b[2J");
    }
}

static int body_lines(void)
{
    int b = rows - 6;
    return b < 1 ? 1 : b;
}

/* Home and End, as mc: the first item, or the last item with the list scrolled
 * as far as it goes. */
static void go_home(struct panel *p)
{
    p->cur = 0;
    p->top = 0;
}

static void go_end(struct panel *p)
{
    int b = body_lines();

    p->cur = p->n ? p->n - 1 : 0;
    p->top = p->n - b > 0 ? p->n - b : 0;
}

/* PgDn and PgUp, with mc's arithmetic (mc's next_page/prev_page): a step of
 * one screenful of items. */
static void page_down(struct panel *p)
{
    int items = body_lines(), n = p->n;

    if (n == 0 || p->cur == n - 1) return;
    if (p->top > n - 2 * items) items = n - items - p->top;
    if (p->top + items < 0) items = -p->top;
    if (items == 0) p->cur = n - 1;
    else p->cur += items;
    p->top += items;
}

static void page_up(struct panel *p)
{
    int items = body_lines();

    if (p->cur == 0 && p->top == 0) return;
    if (p->top < items) items = p->top;
    if (items == 0) p->cur = 0;
    else p->cur -= items;
    p->top -= items;
}

/* Up, Down, Home, End, PgUp and PgDn on the active panel. */
static void nav_key(struct panel *p, int key)
{
    if (key == K_UP) {
        if (p->cur > 0) p->cur--;
    } else if (key == K_DOWN) {
        if (p->cur < p->n - 1) p->cur++;
    } else if (key == K_HOME) {
        go_home(p);
    } else if (key == K_END) {
        go_end(p);
    } else if (key == K_PGUP) {
        page_up(p);
    } else if (key == K_PGDN) {
        page_down(p);
    }
    scroll(p);
}

/* Insert, and Shift+Up or Shift+Down: the highlighted item is marked (or unmarked,
 * if it was marked already), and the cursor moves one item: down for Insert and
 * Shift+Down, up for Shift+Up. ".." is never marked; the cursor stops at either end. */
static void sel_key(struct panel *p, int key)
{
    if (p->n == 0) return;
    if (!p->e[p->ord[p->cur]].up) p->e[p->ord[p->cur]].sel ^= 1;
    if (key == K_SUP) {
        if (p->cur > 0) p->cur--;
    } else if (p->cur < p->n - 1) {
        p->cur++;
    }
    scroll(p);
}

/* The marked items: whether any is marked (a directory counts), and how many marked
 * files and directories there are. ".." is never marked. */
static int marked_any(const struct panel *p)
{
    for (int i = 0; i < p->ne; i++)
        if (p->e[i].sel && !p->e[i].up) return 1;
    return 0;
}

static void marked_counts(const struct panel *p, int *files, int *dirs)
{
    *files = *dirs = 0;
    for (int i = 0; i < p->ne; i++)
        if (p->e[i].sel && !p->e[i].up) {
            if (is_dir(&p->e[i])) (*dirs)++;
            else (*files)++;
        }
}
