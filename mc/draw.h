/* Entry marker followed by the fitted name, in at most W characters. */
static int name_text(char *t, int w, const struct panel *p, const struct ent *e)
{
    t[0] = ent_mark(e);
    return 1 + fit_name(t + 1, p->nm + e->name, w - 1);
}

/* One screen row of one panel, drawn into row[] at column X0 with width PW. */
static void panel_row(struct panel *p, int y, int x0, int pw, int active)
{
    int H = rows - 1, nw = pw - 23, body = H - 5;
    int s1 = x0 + 1 + nw, s2 = s1 + 8, r = x0 + pw - 1;
    uint16_t dec = C_FRAME | DEC;
    char t[300], pb[4096];

    if (nw < 1 || H < 6) return;

    if (y == 0) {                                   /* top border with path */
        int tl = panel_title(pb, sizeof pb, p), tc;
        tl = cut_cols(pb, tl, pw - 12);
        tc = tcols(pb, tl);
        setc(x0, 'l', dec);
        setc(x0 + 1, '<', C_FRAME);
        setc(x0 + 2, 'q', dec);
        uint16_t tt = active ? C_TITLE : C_FRAME;   /* only the active panel's path is highlighted */
        setc(x0 + 3, ' ', tt);
        sets(x0 + 4, pb, tl, tt);
        setc(x0 + 4 + tc, ' ', tt);
        for (int x = x0 + 5 + tc; x < x0 + pw - 6; x++) setc(x, 'q', dec);
        sets(x0 + pw - 6, ".[^]>", 5, C_FRAME);
        setc(r, 'k', dec);
        return;
    }

    if (y == 1) {                                   /* column headers */
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        t[0] = '.';
        t[1] = 'n';
        center(t + 2, "Name", nw - 2);
        sets(x0 + 1, t, nw, C_HEAD);
        setc(s1, 'x', dec);
        center(t, "Size", 7);
        sets(s1 + 1, t, 7, C_HEAD);
        setc(s2, 'x', dec);
        center(t, "Modify time", 12);
        sets(s2 + 1, t, 12, C_HEAD);
        return;
    }

    if (y >= 2 && y <= body + 1) {                  /* entries */
        int k = p->top + (y - 2);
        int hit = active && k == p->cur && k < p->n;
        uint16_t a = C_FRAME, sep = dec, fill = hit ? C_CURSOR : C_FRAME;

        for (int x = x0 + 1; x < r; x++) setc(x, ' ', fill);
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        if (hit) sep = C_CURSOR | DEC;
        setc(s1, 'x', sep);
        setc(s2, 'x', sep);
        if (k < p->n) {
            const struct ent *e = &p->e[p->ord[k]];
            char sz[8];
            int sl;

            a = hit ? (e->sel ? C_SELCUR : C_CURSOR) : e->sel ? C_SEL : cl_attr[e->cls];

            sets(x0 + 1, t, name_text(t, nw, p, e), a);

            if (e->up) sl = cat(sz, "UP--DIR");
            else sl = fmt_size(sz, 7, ent_size(e), 0);
            for (int i = 0; i < 7; i++) t[i] = ' ';
            for (int i = 0; i < sl && i < 7; i++) t[7 - sl + i] = sz[i];
            sets(s1 + 1, t, 7, a);

            fmt_date(t, ent_mtime(e));
            sets(s2 + 1, t, 12, a);
        }
        return;
    }

    if (y == body + 2) {                            /* separator */
        setc(x0, 't', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, 'q', dec);
        setc(r, 'u', dec);
        return;
    }

    if (y == body + 3) {                            /* info line: current entry */
        setc(x0, 'x', dec);
        setc(r, 'x', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, ' ', C_INFO);
        if (active && mode == MODE_QSEARCH) {       /* quick search: the letters typed */
            t[0] = '/';
            for (int k = 0; k < qline.len; k++) t[1 + k] = qline.buf[k];
            sets(x0 + 1, t, 1 + qline.len, C_INFO);
            return;
        }
        if (p->n) {
            const struct ent *e = &p->e[p->ord[p->cur]];
            int n;

            if (e->up) {
                n = cat(t, "UP--DIR");
            } else if (e->link && p->arc[0]) {      /* in an archive: the target, from its index */
                char tg[256];

                n = cat(t, "-> ");
                if (vfs_target(p, p->nm + e->name, tg, sizeof tg))
                    n += fit_name(t + n, tg, pw - 5);
            } else if (e->link) {                   /* "-> target", read now: links are few */
                char tg[256];
                long tl = -1;
                int k = (int)strlcpy(pb, p->path, sizeof pb);

                if (k && pb[k - 1] != '/') strlcat(pb, "/", sizeof pb);
                if (strlcat(pb, p->nm + e->name, sizeof pb) < sizeof pb)
                    tl = readlinkat(AT_FDCWD, pb, tg, sizeof tg - 1);
                n = cat(t, "-> ");
                if (tl > 0) {
                    tg[tl] = 0;
                    n += fit_name(t + n, tg, pw - 5);
                } else {
                    n += cat(t + n, "<readlink failed>");
                }
            } else {
                n = name_text(t, pw - 2, p, e);
            }
            sets(x0 + 1, t, n, C_INFO);
        }
        return;
    }

    if (y == body + 4) {                            /* bottom border with free space */
        setc(x0, 'm', dec);
        setc(r, 'j', dec);
        for (int x = x0 + 1; x < r; x++) setc(x, 'q', dec);
        if (p->total_kb) {                          /* " 243G / 252G (96%) " */
            char avail[8], total[8];
            unsigned long pct = (unsigned long)udiv64(100 * p->free_kb, p->total_kb, NULL);
            int la = fmt_size(avail, 5, p->free_kb, 1);
            int lt = fmt_size(total, 5, p->total_kb, 1);
            int n = 0;

            t[n++] = ' ';
            for (int i = 0; i < la; i++) t[n++] = avail[i];
            n += cat(t + n, "/");
            for (int i = 0; i < lt; i++) t[n++] = total[i];
            n += cat(t + n, " (");
            n += (int)utoa(pct, t + n);
            n += cat(t + n, "%) ");
            sets(x0 + pw - 2 - n, t, n, C_FREE);    /* right-aligned before "qj" */
        }
        return;
    }
}

static void draw(void)
{
    int lw = cols / 2, ax = act == lft ? 0 : lw, aw = act == lft ? lw : cols - lw;
    int ex = 0, ey = 0, cy, cx;
    struct timespec ts;

    if (hidden) {                     /* the normal screen is the terminal's own */
        flush();
        return;
    }
    clock_gettime(CLOCK_REALTIME, &ts);   /* the dates are recent, or not, as of this frame */
    now = ts.tv_sec;
    if (mode != rlast) rfull = 1;     /* a change of mode is a change of the whole screen */
    rlast = mode;

    if (mode == MODE_ERR) err_geom(&ex, &ey);
    if (mode == MODE_QUERY) qy_geom();
    if (mode == MODE_OP || mode == MODE_PROG || mode == MODE_CONFLICT) op_geom();
    if (mode == MODE_SORT) sd_geom();
    if (mode == MODE_LINK) lk_geom();
    if (mode == MODE_GROUP) gr_geom();
    if (mode == MODE_MENU && md && menu_n(mbt)) md_geom();
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            row[x].c = ' ';
            row[x].a = DEFA;
        }
        if (y == rows - 1) {
            prompt_row();
        } else if (!hidden) {
            /* with the menu up, the active panel has no highlight, as mc has it */
            panel_row(&P[lft], y, 0, lw, act == lft && mode != MODE_MENU);
            panel_row(&P[lft ^ 1], y, lw, cols - lw, act == (lft ^ 1) && mode != MODE_MENU);
        }
        if (mode == MODE_MENU) {
            if (y == 0) menu_bar();
            if (md && menu_n(mbt)) md_row(y);
        }
        if (mode == MODE_SORT) sd_row(y);
        if (mode == MODE_LINK) lk_row(y);
        if (mode == MODE_GROUP) gr_row(y);
        if (mode == MODE_QCD) qcd_draw_row(y, ax, aw);
        if (mode == MODE_ERR) err_draw_row(y, ex, ey);
        if (mode == MODE_QUERY) qy_draw_row(y);
        if (mode == MODE_OP || mode == MODE_PROG || mode == MODE_CONFLICT) {
            if (mode == MODE_CONFLICT) cf_row(y - opy);
            else op_row(y - opy);
            if (y >= opy + 1 && y <= opy + oph) shade(opx + opw, opx + opw + 1);
            if (y == opy + oph) shade(opx + 2, opx + opw + 1);
        }
        if (mode == MODE_QUERY) {
            if (y >= qry + 1 && y <= qry + qrh) shade(qrx + qrw, qrx + qrw + 1);
            if (y == qry + qrh) shade(qrx + 2, qrx + qrw + 1);
        }
        /* shadows: to the right of each box, and under it */
        if (mode == MODE_QCD) {
            int top = rows - 2 - QCD_H;

            if (y >= top + 1 && y <= top + QCD_H - 1) shade(ax + aw, ax + aw + 1);
            if (y == top + QCD_H) shade(ax + 2, ax + aw + 1);
        }
        if (mode == MODE_ERR) {
            if (y >= ey + 1 && y <= ey + ERR_H) shade(ex + ewidth, ex + ewidth + 1);
            if (y == ey + ERR_H) shade(ex + 2, ex + ewidth + 1);
        }
        if (mode == MODE_SORT) {
            if (y >= sdy - DIALOG_PAD + 1 && y <= sdy + SD_H + DIALOG_PAD)
                shade(sdx + SD_W + DIALOG_PAD, sdx + SD_W + DIALOG_PAD + 1);
            if (y == sdy + SD_H + DIALOG_PAD) shade(sdx - DIALOG_PAD + 2, sdx + SD_W + DIALOG_PAD + 1);
        }
        if (mode == MODE_LINK) {
            if (y >= lky - DIALOG_PAD + 1 && y <= lky + LK_H + DIALOG_PAD)
                shade(lkx + LK_W + DIALOG_PAD, lkx + LK_W + DIALOG_PAD + 1);
            if (y == lky + LK_H + DIALOG_PAD) shade(lkx - DIALOG_PAD + 2, lkx + LK_W + DIALOG_PAD + 1);
        }
        if (mode == MODE_MENU && md && menu_n(mbt)) {
            if (y >= 2 && y <= mdh + 1) shade(mdx + mdw, mdx + mdw + 1);
            if (y == mdh + 1) shade(mdx + 2, mdx + mdw + 1);
        }
        /* While a progress box is up, and nothing else has changed, only its columns are
         * written: its place and size are fixed for the whole job. Any other draw, or
         * the first after a change of mode, writes every row. */
        if (rfull || mode != MODE_PROG) emit_row(y, 0, cols);
        else if (y >= opy && y <= opy + oph) emit_row(y, opx, opx + opw + 2 < cols ? opx + opw + 2 : cols);
    }
    rfull = 0;

    /* cursor: in the quick cd field, otherwise on the command line */
    if (mode == MODE_QCD) {
        cy = rows - 4 - DIALOG_PAD;
        cx = ax + DIALOG_PAD + 5 + line_cells(&qline);
    } else if (mode == MODE_OP) {
        cy = opy + DIALOG_PAD + 2;
        cx = opx + DIALOG_PAD + 2 + line_cells(&dline);
    } else {
        cy = rows - 1;
        cx = prompt_len + line_cells(&cmdl);
        if (cx > cols - 2) cx = cols - 2;
    }
    outs("\x1b[");
    outu((unsigned long)cy + 1);
    outs(";");
    outu((unsigned long)cx + 1);
    outs("H");
    flush();
}
