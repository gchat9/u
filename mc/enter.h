/* enter.h - Enter, F3 and the command line on an item: entering it, running it,
 * showing it, or running what was typed. */

/* The viewer for a media file, as mc.ext.ini starts it for the same types: the
 * script, in /usr/lib/mc/ext.d, and its file type, in TYPE.  NULL if the file
 * is not a media file.  The extension is matched without regard to case. */
static const char *media_script(const char *name, char *type)
{
    static const char sound[] = "/usr/lib/mc/ext.d/sound.sh";
    static const char video[] = "/usr/lib/mc/ext.d/video.sh";
    static const char image[] = "/usr/lib/mc/ext.d/image.sh";
    const char *dot = NULL, *s;
    size_t n;
    int k;

    for (s = name; *s; s++)
        if (*s == '.') dot = s;
    if (!dot || !dot[1] || (n = strlen(dot + 1)) >= 8) return NULL;
    for (k = 0; k < (int)n; k++) type[k] = (char)lc(dot[1 + k]);
    type[n] = 0;
    if (ext_in("mp3;ogg;opus", type)) return sound;
    if (ext_in("m4a", type)) {
        strlcpy(type, "common", 16);
        return sound;
    }
    if (ext_in("mp4;webm;mkv", type)) {
        strlcpy(type, "ALL_FORMATS", 16);
        return video;
    }
    if (ext_in("gif;png;webp;jpg;jpeg", type)) {
        strlcpy(type, "ALL_FORMATS", 16);
        return image;
    }
    return NULL;
}

/* Enter on a media file: its viewer runs as mc runs a program: the panels go,
 * the viewer has the keys, and the panels come back when it has ended.  The
 * file is in MC_EXT_FILENAME, as the viewers expect. */
static void media_play(struct panel *p, const struct ent *e, const char *script, const char *type)
{
    static const char sh_script[] = "MC_EXT_FILENAME=\"$2\" exec \"$0\" open \"$1\"";
    char file[4352], *argv[7];
    int k;

    k = cat(file, p->path);
    if (k && file[k - 1] != '/') file[k++] = '/';
    k += cat(file + k, p->nm + e->name);
    file[k] = 0;
    argv[0] = (char *)"sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)sh_script;
    argv[3] = (char *)script;
    argv[4] = (char *)type;
    argv[5] = file;
    argv[6] = NULL;
    run_child("/bin/sh", argv, p->path, RUN_PAUSE, NULL);
}

/* Enter on the highlighted item: a directory is entered, an archive is opened, a
 * media file is played, an executable file is run. Other files do nothing. */
static void enter_item(struct panel *p)
{
    const struct ent *e;
    char *argv[2], dot[300];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (is_dir(e)) {
        enter_dir(p);
        return;
    }
    if (p->arc[0]) return;                      /* inside an archive: not extracted yet */
    if (archive_kind(p->nm + e->name) != AR_NONE) {
        vfs_open(p, e);
        return;
    }
    if (!is_exe(e)) {
        char type[16];
        const char *script = media_script(p->nm + e->name, type);

        if (script) media_play(p, e, script, type);
        return;
    }
    dot[0] = '.';
    dot[1] = '/';
    dot[2] = 0;
    strlcat(dot, p->nm + e->name, sizeof dot);
    argv[0] = dot;
    argv[1] = NULL;
    run_child(dot, argv, p->path, 0, NULL);
}

/* F3 on a file of the archive panel P: the file is written to a staging directory of
 * its own, mcview runs on it, and the directory goes when mcview has exited. */
static void vfs_view(struct panel *p, const struct ent *e)
{
    char inner[512], path[4352], *argv[3];
    struct statx sx;
    long r;

    strlcpy(inner, p->sub, sizeof inner);
    strlcat(inner, p->nm + e->name, sizeof inner);
    xo.n = 0;
    xo.used = 0;
    xo.err = 0;
    r = xtmp_make();
    if (!r) r = xo_add(inner);
    if (!r) {
        vfs_hard_targets(p);
        r = archive_scan(p, NULL, &xo);
    }
    if (!r && join(path, sizeof path, xo.root, inner)) r = -ENAMETOOLONG;
    if (!r && (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx) != 0 ||
               (sx.stx_mode & S_IFMT) != S_IFREG))
        r = -ENOENT;
    if (r) {
        set_error("Cannot extract", inner, -r);
    } else {
        argv[0] = (char *)"mcview";
        argv[1] = path;
        argv[2] = NULL;
        run_child("mcview", argv, xo.root, RUN_SEARCH, NULL);
    }
    if (xo.root[0]) del_tree(xo.root, 1);
    xo.root[0] = 0;
}

/* F3 on a file of the link: the file is fetched into a staging directory of its own,
 * and mcview runs on that copy. Nothing on this machine with the same path is read. */
static void remote_view(struct panel *p, const struct ent *e)
{
    char remote[4352], local[4352], *argv[3];
    long r = xtmp_make();
    int fd;

    if (!r) r = join(remote, sizeof remote, p->path, p->nm + e->name);
    if (!r && join(local, sizeof local, xo.root, p->nm + e->name)) r = -ENAMETOOLONG;
    if (!r) {
        fd = (int)open(local, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        r = fd < 0 ? fd : remote_fetch(remote, fd);
        if (fd >= 0) close(fd);
    }
    if (r) {
        set_error("Cannot fetch", p->nm + e->name, -r);
    } else {
        argv[0] = (char *)"mcview";
        argv[1] = local;
        argv[2] = NULL;
        run_child("mcview", argv, xo.root, RUN_SEARCH, NULL);
    }
    if (xo.root[0]) del_tree(xo.root, 1);
    xo.root[0] = 0;
}

static void view_item(struct panel *p)
{
    const struct ent *e;
    char *argv[3];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (is_dir(e)) {
        enter_dir(p);
        return;
    }
    if (p->arc[0]) {
        vfs_view(p, e);
        return;
    }
    if (p->rem) {
        remote_view(p, e);
        return;
    }
    argv[0] = (char *)"mcview";
    argv[1] = (char *)(p->nm + e->name);
    argv[2] = NULL;
    run_child("mcview", argv, p->path, RUN_SEARCH, NULL);
}

static void run_command(void)
{
    char cmd[256], *argv[4];
    const char *sh = genv("SHELL");
    int k;

    if (!sh || !*sh) sh = "/bin/sh";
    for (k = 0; k <= cmdl.len; k++) cmd[k] = cmdl.buf[k];
    line_clear(&cmdl);
    cmd_hist = 1;
    /* "cd DIR" is Alt+C DIR: a child process cannot change the panel's directory */
    if (cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == 0 || cmd[2] == ' ')) {
        quick_cd(cmd + 2);
        return;
    }
    argv[0] = (char *)sh;
    argv[1] = (char *)"-c";
    argv[2] = cmd;
    argv[3] = NULL;
    run_child(sh, argv, P[act].path, 0, cmd);
}

/* Alt+Enter: the highlighted name goes on the command line, at the cursor, and
 * a space after it. The name is quoted as mc quotes it: a backslash before each
 * character the shell would read as special, and before a leading ~ or #; a
 * leading - gets ./ in front, so it is not an option. A name that does not fit
 * on the line is not typed at all. */
static void put_name(const struct panel *p)
{
    const struct ent *e;
    const char *s;
    char t[520];
    int n = 0, j;

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    s = p->nm + e->name;
    if (*s == '-') {
        t[n++] = '.';
        t[n++] = '/';
    }
    for (; *s && n < (int)sizeof t - 4; s++) {
        int c = (unsigned char)*s, esc = 0;

        for (const char *q = "\t\r\n \"'\\;?|[]{}<>`!$&*()"; *q; q++)
            if (*q == c) esc = 1;
        if (n == 0 && (c == '~' || c == '#')) esc = 1;
        if (esc) t[n++] = '\\';
        t[n++] = (char)c;
    }
    t[n++] = ' ';
    if (cmdl.len + n >= (int)sizeof cmdl.buf) return;
    for (j = 0; j < n; j++) line_insert(&cmdl, (unsigned char)t[j]);
}
