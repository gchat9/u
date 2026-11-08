/* ------------------------------------------------ copy and move (F5, F6)
 *
 * A file is copied or moved in two steps, as mc does it: a question, with the
 * destination typed in a field (Enter does it, Esc gives up), and then a
 * progress box while the bytes go across. The destination starts as the other
 * panel's directory. */

static char op_t1[320];                 /* the question's text: Copy "name" to: */
static int op_n1;
static struct line dline;               /* the destination, as typed */
static int opx, opy, opw, oph;          /* the box: column, row, width, height */
static const char *op_title[2] = { " Copy ", " Move " };

/* F8 or ESC 8: the question. For marked items it counts them, worded as mc words it:
 * "files", "directories", or "files/directories" when both are there. For the
 * highlighted item it names it: "Delete directory" or "Delete file", and the name. */
static void delete_open(struct panel *p)
{
    const struct ent *e;
    char fitted[256];
    int n, files, dirs;

    if (p->arc[0]) {                                /* read-only: nothing is deleted */
        set_error("Read-only archive", p->arc, EROFS);
        return;
    }
    strlcpy(job_base, p->path, sizeof job_base);    /* the items are in the panel's directory */

    job.sel = marked_any(p);
    if (job.sel) {
        marked_counts(p, &files, &dirs);
        if (files + dirs == 0) return;
        qy_n1 = cat(qy_t1, "Delete ");
        if (files + dirs == 1) {
            qy_n1 += cat(qy_t1 + qy_n1, dirs ? "directory" : "file");
        } else {
            qy_n1 += (int)utoa((unsigned long)(files + dirs), qy_t1 + qy_n1);
            qy_n1 += cat(qy_t1 + qy_n1, dirs == 0 ? " files" : files == 0 ? " directories"
                                                              : " files/directories");
        }
        qy_n1 += cat(qy_t1 + qy_n1, "?");
        qy_n2 = 0;
    } else {
        if (p->n == 0) return;
        e = &p->e[p->ord[p->cur]];
        if (e->up) return;
        n = fit_name(fitted, p->nm + e->name, 43);
        qy_n1 = cat(qy_t1, is_dir(e) ? "Delete directory" : "Delete file");
        qy_n2 = 0;
        qy_t2[qy_n2++] = '"';
        for (int k = 0; k < n; k++) qy_t2[qy_n2++] = fitted[k];
        qy_t2[qy_n2++] = '"';
        qy_t2[qy_n2++] = '?';
    }
    qy_kind = QY_ASK;
    qy_sel = 0;
    mode = MODE_QUERY;
}

/* The choice of a button. Yes in the Delete question starts the job. Every other
 * question (a failure, a directory) only records its answer: the job waiting for it
 * goes on. */
static void qy_choose(int b)
{
    if (qy_kind == QY_ASK) {
        mode = MODE_NORMAL;
        if (b == 0) {
            job.kind = JOB_DEL;
            job.dir = 0;
            job_run();
        }
        return;
    }
    qy_ans = b;
    mode = MODE_NORMAL;
}

/* The keys of a question: its hotkeys, Enter for the selected button, Left and
 * Right (or Tab) to choose, and Esc, which is No or Abort. */
static int qy_handle(int key)
{
    int nb;
    const struct qy_button *b = qy_buttons(&nb);

    if (key == K_ESC) {
        qy_choose(qy_kind == QY_ASK ? 1 : nb - 1);
        return 0;
    }
    if (key == 13 || key == 10) {
        qy_choose(qy_sel);
        return 0;
    }
    if (key == K_LEFT || key == K_RIGHT || key == 9) {
        qy_sel = key == K_LEFT ? (qy_sel + nb - 1) % nb : (qy_sel + 1) % nb;
        return 0;
    }
    for (int i = 0; i < nb; i++) {
        int h = (unsigned char)b[i].label[b[i].hot], k = key;

        if (h >= 'A' && h <= 'Z') h += 32;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (k == h) {
            qy_choose(i);
            return 0;
        }
    }
    return 0;
}

/* The box: 64 cells wide, or less on a narrow screen, and in the upper third of
 * the screen. The question is six rows, the progress box nine. */
static void op_geom(void)
{
    opw = cols - 4 < 64 ? cols - 4 : 64;
    if (opw < 8) opw = 8;
    oph = (mode == MODE_PROG ? 10 : mode == MODE_CONFLICT ? CF_H : 6) + 2 * DIALOG_PAD;
    opx = (cols - opw) / 2;
    opy = (rows - oph) / 3;
    if (opx < 0) opx = 0;
    if (opy < 0) opy = 0;
}

/* The progress bar: [=====     ] and the percentage PCT, in W cells at X. */
static void op_bar(int x, int w, int pct)
{
    int bw = w - 9, filled = pct * bw / 100;
    char pc[4];

    if (bw < 1) return;
    setc(x, '[', C_QCD);
    for (int i = 0; i < bw; i++) setc(x + 1 + i, ' ', i < filled ? C_FIELD : C_QCD);
    setc(x + 1 + bw, ']', C_QCD);
    putnum(pc, (unsigned long)pct, 3);
    setc(x + 2 + bw, ' ', C_QCD);
    sets(x + 3 + bw, pc, 3, C_QCD);
    setc(x + 6 + bw, '%', C_QCD);
}

/* One row of the box.  The question: its text, the destination field, and the
 * buttons.  The progress box: source and destination, the bar and [ Abort ]. */
static void op_row(int dy)
{
    int prog = mode == MODE_PROG, fh = oph - 2 * DIALOG_PAD;
    int x0 = opx + DIALOG_PAD, r = opx + opw - 1 - DIALOG_PAD, in = OPFW - 4, x;
    int sep = prog ? 7 : 3, btn = prog ? 8 : 4;

    if (dy < 0 || dy >= oph) return;
    for (x = opx; x < opx + opw; x++) setc(x, ' ', C_QCD);     /* the margin too */
    dy -= DIALOG_PAD;
    if (dy < 0 || dy >= fh) return;
    if (dy == 0 || dy == fh - 1) {                       /* the frame, titled on top */
        setc(x0, dy ? 'm' : 'l', C_QCD | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        setc(r, dy ? 'j' : 'k', C_QCD | DEC);
        if (!dy) sets(x0 + (OPFW - 6) / 2, op_title[job.kind == JOB_COPY ? 0 : 1], 6, C_QTITLE);
        return;
    }
    setc(x0, dy == sep ? 't' : 'x', C_QCD | DEC);
    setc(r, dy == sep ? 'u' : 'x', C_QCD | DEC);
    if (dy == sep) {
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        return;
    }
    if (dy == btn) {
        if (prog) {
            qy_label(x0 + 2 + (in - 9) / 2, "[ Abort ]", 2, C_FIELD, C_QHOTSEL);
        } else {
            x = x0 + 2 + (in - 18) / 2;
            sets(x, "[ OK ]", 6, C_FIELD);
            sets(x + 8, "[ Cancel ]", 10, C_QCD);
        }
        return;
    }
    if (!prog) {
        if (dy == 1) sets(x0 + 2, op_t1, op_n1, C_QCD);
        if (dy == 2) {
            line_view(&dline, in, 0);
            line_draw(&dline, x0 + 2, in, C_FIELD);
        }
        return;
    }
    if (dy == 1) sets(x0 + 2, "Source", 6, C_QCD);
    if (dy == 2) show_tail(x0 + 2, pg_src, (int)strlen(pg_src), in, C_QCD);
    if (dy == 3) sets(x0 + 2, "Target", 6, C_QCD);
    if (dy == 4) show_tail(x0 + 2, pg_dst, (int)strlen(pg_dst), in, C_QCD);
    if (dy == 5) op_bar(x0 + 2, in, pg_pct > 0 ? pg_pct : 0);
    if (dy == 6) {
        sets(x0 + 2, "Total", 5, C_QCD);
        op_bar(x0 + 8, in - 6, pct_of(pt_now, pt_total));
    }
}

/* The overwrite question, as mc asks it: the new file and the one that exists,
 * with sizes and dates; Yes, No and Append for this file; and for the rest of the
 * job All, Older, None, Smaller and Size differs, with a checkbox for zero-length
 * files, and Abort. Items 0 to 9 are the buttons in focus order. */
static const char *const cf_label[10] = {
    "[ Yes ]", "[ No ]", "[ Append ]", "", "[ All ]", "[ Older ]", "[ None ]",
    "[ Smaller ]", "[ Size differs ]", "[ Abort ]"
};
static const char cf_hot[10] = { 'y', 'n', 'p', 'z', 'l', 'o', 'e', 'm', 's', 'a' };
static const unsigned char cf_at[10] = { 7, 7, 7, 10, 11, 11, 11, 11, 11, 13 };   /* each item's row */

static const char *cf_str(int i)
{
    if (i == 3) return cf_zero ? "[x] Don't overwrite with zero length file"
                               : "[ ] Don't overwrite with zero length file";
    return cf_label[i];
}

/* The index in S of its first letter HOT, either case, or -1: the hotkey's letter. */
static int hot_at(const char *s, int hot)
{
    for (int k = 0; s[k]; k++) {
        int c = (unsigned char)s[k];

        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == hot) return k;
    }
    return -1;
}

/* The buttons of row DY, centred in the IN cells after X0. */
static void cf_buttons(int dy, int x0, int in)
{
    int items[10], n = 0, w = 0, x;

    for (int i = 0; i < 10; i++)
        if (cf_at[i] == dy) items[n++] = i;
    for (int j = 0; j < n; j++) w += (int)strlen(cf_str(items[j])) + (j ? 1 : 0);
    x = x0 + 1 + (in - w) / 2;
    for (int j = 0; j < n; j++) {
        const char *s = cf_str(items[j]);
        int sel = items[j] == cf_sel;

        qy_label(x, s, hot_at(s, cf_hot[items[j]]), sel ? C_TITLE : C_ERR,
                 sel ? ATTR(BROWN, WHITE, 1) : C_ERRHEAD);
        x += (int)strlen(s) + 1;
    }
}

/* One row of the question. */
static void cf_row(int dy)
{
    int x0 = opx + DIALOG_PAD, r = opx + opw - 1 - DIALOG_PAD, x, in = OPFW - 2, l;
    char sz[16], tm[16];

    if (dy < 0 || dy >= oph) return;
    for (x = opx; x < opx + opw; x++) setc(x, ' ', C_ERR);     /* the margin too */
    dy -= DIALOG_PAD;
    if (dy < 0 || dy >= CF_H) return;
    if (dy == 0 || dy == CF_H - 1) {
        setc(x0, dy ? 'm' : 'l', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, dy ? 'j' : 'k', C_ERR | DEC);
        if (!dy) sets(x0 + (OPFW - 13) / 2, " File exists ", 13, C_ERRHEAD);
        return;
    }
    if (dy == 5 || dy == 8 || dy == 12) {
        setc(x0, 't', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'u', C_ERR | DEC);
        return;
    }
    setc(x0, 'x', C_ERR | DEC);
    setc(r, 'x', C_ERR | DEC);
    switch (dy) {
    case 1:
    case 3: {
        const char *p = dy == 1 ? cf_srcp : cf_dstp;

        sets(x0 + 2, dy == 1 ? "New     :" : "Existing:", 9, C_ERR);
        show_tail(x0 + 12, p, (int)strlen(p), r - x0 - 12, C_ERR);
        break;
    }
    case 2:
    case 4: {
        uint64_t size = dy == 2 ? cf_ssize : cf_dsize;
        int64_t mt = dy == 2 ? cf_smt : cf_dmt;

        l = fmt_size(sz, 7, size, 0);
        sets(x0 + 12, sz, l, C_ERR);
        fmt_date(tm, mt);
        sets(r - 13, tm, 12, C_ERR);
        break;
    }
    case 6:
        sets(x0 + 1 + (in - 20) / 2, "Overwrite this file?", 20, C_ERR);
        break;
    case 9:
        sets(x0 + 1 + (in - 20) / 2, "Overwrite all files?", 20, C_ERR);
        break;
    default:
        cf_buttons(dy, x0, in);
        break;
    }
}

/* The question's choice of button I. The checkbox just toggles; Abort ends the
 * job; the per-file answers decide this file; a group answer stands for the rest. */
static void cf_choose(int i)
{
    cf_sel = i;
    if (i == 3) {                       /* the checkbox just toggles */
        cf_zero ^= 1;
        return;
    }
    cf_ans = i;
    mode = MODE_NORMAL;
}

/* The keys of the question: Enter for the focused button, Left and Right (and
 * Tab) to move the focus, the hotkeys, and Esc for Abort. */
static int cf_handle(int key)
{
    if (key == K_ESC) {
        cf_choose(9);
        return 0;
    }
    if (key == '\r' || key == '\n') {
        cf_choose(cf_sel);
        return 0;
    }
    if (key == K_LEFT) {
        cf_sel = (cf_sel + 9) % 10;
        return 0;
    }
    if (key == K_RIGHT || key == 9) {
        cf_sel = (cf_sel + 1) % 10;
        return 0;
    }
    if (key >= 'A' && key <= 'Z') key += 32;
    for (int i = 0; i < 10; i++)
        if (key == cf_hot[i]) {
            cf_choose(i);
            return 0;
        }
    return 0;
}

/* Enter in the question: the job starts. A destination that is a directory takes
 * each item under its own name; marked items need a directory to go to. An empty
 * destination does nothing. */
static void op_confirm(void)
{
    struct panel *p = &P[act], *d;
    char res[4096], shown[512];
    struct statx sx;
    int isdir;

    if (!dline.len) {
        mode = MODE_NORMAL;
        return;
    }
    d = &P[act ^ 1];
    path_of(dline.buf, d->rem ? d->path : p->path, res, sizeof res, shown, sizeof shown);
    if (d->rem) isdir = remote_isdir(res) == 1;
    else isdir = statx(AT_FDCWD, res, 0, STATX_BASIC_STATS, &sx) == 0 &&
                 (sx.stx_mode & S_IFMT) == S_IFDIR;
    if (job.sel && !isdir) {
        mode = MODE_NORMAL;
        set_error(job.kind == JOB_COPY ? "Cannot copy to" : "Cannot move to", res, ENOTDIR);
        return;
    }
    strlcpy(job_dst, res, sizeof job_dst);
    job.dir = isdir;
    if (p->arc[0]) {
        long r = vfs_stage(p, job.sel);

        if (r) {
            if (xo.root[0]) del_tree(xo.root, 1);
            xo.root[0] = 0;
            mode = MODE_NORMAL;
            set_error("Cannot extract from", p->arc, -r);
            return;
        }
        strlcpy(job_base, xo.root, sizeof job_base);
        job_begin(p->path, job_dst);
        pt_tarish = 0;
        scan_items(p, xo.root, job.sel, 0);
        pg_draw(1);
        job_run();
        del_tree(xo.root, 1);
        xo.root[0] = 0;
        return;
    }
    if (p->rem || d->rem) {
        remote_job(p, d);
        return;
    }
    strlcpy(job_base, p->path, sizeof job_base);
    job_begin(p->path, job_dst);
    pt_tarish = 0;
    scan_items(p, p->path, job.sel, 0);
    pg_draw(1);
    job_run();
}

/* F5 or F6: the question. For marked items it gives their number, else it names the
 * highlighted item, as a file or a directory. The destination starts as the other
 * panel's directory. Nothing is asked when nothing is marked and the highlight is on
 * "..". */
static void op_open(int kind)
{
    struct panel *p = &P[act];
    const struct ent *e = NULL;
    const char *name = "";
    int n, k, l, files, dirs;

    if (kind == JOB_MOVE && p->arc[0]) {
        set_error("Read-only archive", p->arc, EROFS);
        return;
    }
    if (P[act ^ 1].arc[0]) {                        /* nothing is written into an archive */
        set_error("Read-only archive", P[act ^ 1].arc, EROFS);
        return;
    }

    job.kind = kind;
    job.sel = marked_any(p);
    if (job.sel) {
        marked_counts(p, &files, &dirs);
        if (files + dirs == 0) return;
    } else {
        if (!p->n) return;
        e = &p->e[p->ord[p->cur]];
        if (e->up) return;
        name = p->nm + e->name;
    }
    mode = MODE_OP;
    op_geom();
    n = cat(op_t1, kind == JOB_COPY ? "Copy " : "Move ");
    if (job.sel) {
        n += (int)utoa((unsigned long)(files + dirs), op_t1 + n);
        n += cat(op_t1 + n, files + dirs == 1 ? " file to:" : " files to:");
    } else {
        if (is_dir(e)) n += cat(op_t1 + n, "directory ");
        n += cat(op_t1 + n, "\"");
        k = cut_cols(name, (int)strlen(name), opw - 26);
        for (int i = 0; i < k; i++) op_t1[n++] = name[i];
        n += cat(op_t1 + n, "\" to:");
    }
    op_n1 = n;
    line_clear(&dline);
    l = (int)strlen(P[act ^ 1].path);
    if (l + 2 < (int)sizeof dline.buf) {
        cat(dline.buf, P[act ^ 1].path);
        if (dline.buf[l - 1] != '/') dline.buf[l++] = '/';
        dline.buf[l] = 0;
        dline.len = dline.pos = l;
    }
}

/* Keys in the question: Enter does it, Esc gives up, the rest edits the
 * destination. */
static int op_handle(int key)
{
    if (key == K_ESC) mode = MODE_NORMAL;
    else if (key == '\r' || key == '\n') op_confirm();
    else line_key(&dline, key, OPFW - 4, 0);
    return 0;
}
