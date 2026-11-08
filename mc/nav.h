/* ------------------------------------------------------------- quick cd */

/* The absolute path TYPED names, as quick cd takes it: blanks around it are
 * dropped, an empty entry or ~ is $HOME, ~USER is that account's home, and a
 * relative path is taken from BASE. RES gets the path, SHOWN the path as typed,
 * for the error box. */
static void path_of(const char *typed, const char *base, char *res, size_t rmax,
                    char *shown, size_t smax)
{
    char s[256], abs[4096];
    const char *home = genv("HOME");
    size_t a = 0, b, n = 0;

    b = strlen(typed);
    while (a < b && (typed[a] == ' ' || typed[a] == '\t')) a++;
    while (b > a && (typed[b - 1] == ' ' || typed[b - 1] == '\t' ||
                     typed[b - 1] == '\r' || typed[b - 1] == '\n')) b--;
    for (size_t i = a; i < b; i++) s[n++] = typed[i];
    s[n] = 0;

    if (n == 0) {
        strlcpy(shown, home ? home : "/", smax);
    } else if (s[0] == '~') {                      /* ~ and ~/ : the account's home */
        size_t k = 1;
        char hb[4096];
        int ok;

        while (s[k] && s[k] != '/') k++;
        ok = k == 1 ? pw_field(NULL, 0, uid, 5, hb, sizeof hb)
                    : pw_field(s + 1, k - 1, 0, 5, hb, sizeof hb);
        if (ok) {
            strlcpy(shown, hb, smax);
            strlcat(shown, s + k, smax);
        } else {
            strlcpy(shown, s, smax);
        }
    } else {
        strlcpy(shown, s, smax);
    }

    if (shown[0] == '/') {
        strlcpy(abs, shown, sizeof abs);
    } else {
        strlcpy(abs, base, sizeof abs);
        if (abs[strlen(abs) - 1] != '/') strlcat(abs, "/", sizeof abs);
        strlcat(abs, shown, sizeof abs);
    }
    canon(res, rmax, abs);
}

/* Change panel P to the absolute directory RES. SHOWN is the path as given,
 * for the error box. Going up selects the directory we came from; anything
 * else starts at the top of the list. */
static void change_to(struct panel *p, const char *res, const char *shown)
{
    char old[4096], dir[4096];
    int was = p->rem, rc;

    if (is_link(res)) {                         /* a shell link: its directory is the path */
        if (shell_connect(p, res, dir, sizeof dir) != 0) return;
        if (!was && !P[(p - P) ^ 1].rem) strlcpy(rorig, p->path, sizeof rorig);
        p->rem = 1;
        res = shown = dir;
    } else if (!p->rem) {
        long fd = open(res, O_RDONLY | O_DIRECTORY, 0);

        if (fd < 0) {
            set_error("Cannot change directory to", shown, -fd);
            return;
        }
        close((int)fd);
        vfs_forget(p);                          /* a real directory: out of any archive */
    }
    strlcpy(old, p->path, sizeof old);
    strlcpy(p->path, res, sizeof p->path);
    rc = load(p);
    if (rc && p->rem) {                         /* the link has no such directory: stay */
        strlcpy(p->path, old, sizeof p->path);
        p->rem = (uint8_t)was;
        load(p);
        rdrop();
        set_error("Cannot change directory to", shown, -rc);
        return;
    }
    vfs_forget(p);                              /* a link: out of any archive too */
    p->cur = 0;
    p->top = 0;
    /* As mc: going up selects the directory we came from, and the window is
     * put around it without being clamped to the end of the list. */
    {
        size_t nl = strlen(res), k, m = 0;
        int up = strlen(old) > nl && (nl == 1 || old[nl] == '/');
        char name[256];

        for (k = 0; up && k < nl; k++)
            if (old[k] != res[k]) up = 0;
        if (up) {
            k = nl == 1 ? 1 : nl + 1;
            while (old[k] && old[k] != '/' && m < sizeof name - 1) name[m++] = old[k++];
            name[m] = 0;
            for (int i = 1; i < p->n; i++) {
                const char *en = p->nm + p->e[p->ord[i]].name;
                size_t j = 0;

                while (name[j] && name[j] == en[j]) j++;
                if (name[j] == 0 && en[j] == 0) {
                    int t = i - (rows - 3) / 2, last = p->n - body_lines();

                    p->cur = i;
                    if (t > last) t = last;             /* the window stops at the last full screen */
                    p->top = t > 0 ? t : 0;
                    break;
                }
            }
        }
    }
}

/* Change the active panel to the directory the user typed. As in mc: blanks
 * around the text are dropped, an empty entry means $HOME, "~" is $HOME, and a
 * relative path is taken from the panel's directory. */
static void quick_cd(const char *typed)
{
    char res[4096], shown[512];

    while (*typed == ' ') typed++;              /* "cd DIR": the space after cd */
    if (is_link(typed)) {                       /* a shell link: a URL, not a path */
        change_to(&P[act], typed, typed);
        return;
    }
    path_of(typed, P[act].path, res, sizeof res, shown, sizeof shown);
    change_to(&P[act], res, shown);
}

/* Keep the cursor visible in the quick cd field, as mc's input widget does:
 * the field is the panel width less 9 (its 3-column [^] button is inside
 * that), and when the cursor leaves it the text moves so the cursor is a
 * third of the width in from the left. */
static int act_pw(void)
{
    return act == lft ? cols / 2 : cols - cols / 2;
}

/* Keys in the quick cd box. Enter goes to the directory typed; Esc closes. */
static void qcd_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return;
    }
    if (key == '\r' || key == '\n') {
        char t[256];
        int k;

        for (k = 0; k <= qline.len; k++) t[k] = qline.buf[k];
        mode = MODE_NORMAL;
        if (qline.len) quick_cd(t);
        return;
    }
    line_key(&qline, key, act_pw() - 9, 3);
}

/* The highlight on the entry called NAME, if the list has one. */
static void select_name(struct panel *p, const char *name)
{
    p->cur = 0;
    p->top = 0;
    for (int i = 0; i < p->n; i++)
        if (!scmp(p->nm + p->e[p->ord[i]].name, name)) {
            p->cur = i;
            break;
        }
    scroll(p);
}

/* Enter the archive named by E, a file of the panel's directory: the panel shows the
 * archive's root. An archive that cannot be read is an error, and nothing changes. */
static void vfs_open(struct panel *p, const struct ent *e)
{
    char name[256];
    int rc;

    strlcpy(name, p->nm + e->name, sizeof name);    /* load() rewrites the names */
    strlcpy(p->arc, name, sizeof p->arc);
    p->sub[0] = 0;
    rc = load(p);
    if (rc != 0) {
        vfs_forget(p);
        load(p);
        set_error("Cannot open archive", name, -rc);
        return;
    }
    p->cur = 0;
    p->top = 0;
    scroll(p);
}

/* Enter the directory E of the archive shown. */
static void vfs_enter(struct panel *p, const struct ent *e)
{
    char name[256];

    if (strlen(p->sub) + strlen(p->nm + e->name) + 2 > sizeof p->sub) return;
    strlcpy(name, p->nm + e->name, sizeof name);    /* load() rewrites the names */
    strlcat(p->sub, name, sizeof p->sub);
    strlcat(p->sub, "/", sizeof p->sub);
    load(p);
    p->cur = 0;
    p->top = 0;
    scroll(p);
}

/* ".." in an archive: up one directory inside it, its highlight on the directory
 * left; at the archive's root, out of the archive, its highlight on the archive. */
static void vfs_up(struct panel *p)
{
    char name[256];
    size_t n, st;

    if (!p->sub[0]) {
        strlcpy(name, p->arc, sizeof name);
        vfs_forget(p);
        load(p);
        select_name(p, name);
        return;
    }
    n = strlen(p->sub) - 1;                         /* the last / */
    st = n;
    while (st > 0 && p->sub[st - 1] != '/') st--;   /* the directory left is sub[st..n) */
    strlcpy(name, p->sub + st, sizeof name);
    name[n - st] = 0;
    p->sub[st] = 0;
    load(p);
    select_name(p, name);
}

/* Enter on a directory, or on "..": change the panel to it. Files do nothing. */
static void enter_dir(struct panel *p)
{
    const struct ent *e;
    char abs[4096], res[4096];

    if (!p->n) return;
    e = &p->e[p->ord[p->cur]];
    if (!is_dir(e)) return;
    if (p->rem && e->up && p->path[0] == '/' && p->path[1] == 0) {
        char back[4096];

        strlcpy(back, rorig, sizeof back);
        p->rem = 0;
        change_to(p, back, back);
        if (scmp(p->path, back) != 0) p->rem = 1;   /* that is gone: the link stays */
        else rdrop();
        return;
    }
    if (p->arc[0]) {                                /* inside an archive */
        if (e->up) vfs_up(p);
        else vfs_enter(p, e);
        return;
    }
    strlcpy(abs, p->path, sizeof abs);
    if (e->up) {
        strlcat(abs, "/..", sizeof abs);
    } else {
        if (abs[strlen(abs) - 1] != '/') strlcat(abs, "/", sizeof abs);
        strlcat(abs, p->nm + e->name, sizeof abs);
    }
    canon(res, sizeof res, abs);
    change_to(p, res, res);
}
