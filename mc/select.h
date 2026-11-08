/* ----------------------------------------------------------- group selection */

/* + and - ask for a shell pattern, and select or unselect every entry that matches it,
 * directories too, but not "..": mc's defaults, case sensitive. * inverts the marks of
 * the files, leaving the directories as they are. The dialog offers the last pattern. */

/* One bracket expression of a pattern, [..] at *P (after its [), matching C: 1 if it
 * does, 0 if not; *P moves past it. A "!" or "^" first negates it. */
static int glob_bracket(const char **p, int c)
{
    int neg = **p == '!' || **p == '^', hit = 0;

    if (neg) (*p)++;
    for (int first = 1; **p && (**p != ']' || first); first = 0) {
        int lo = (unsigned char)*(*p)++, hi = lo;

        if (**p == '-' && (*p)[1] && (*p)[1] != ']') {
            (*p)++;
            hi = (unsigned char)*(*p)++;
        }
        if (c >= lo && c <= hi) hit = 1;
    }
    if (**p == ']') (*p)++;
    return hit != neg;
}

/* Whether the name S matches the shell pattern P: * any run, ? any one character,
 * [..] one of a set, and anything else as itself.  After a * that has failed, the
 * match goes on from the last *, one character further: the earlier ones need no
 * second look, so the time is that of the name times the pattern, not more. */
static int glob_match(const char *p, const char *s)
{
    const char *star = NULL, *back = NULL;

    for (;;) {
        int ok = 0;

        if (*p == '*') {
            star = ++p;
            back = s;
            continue;
        }
        if (!*s) {
            if (!*p) return 1;
        } else if (*p == '?') {
            p++;
            ok = 1;
        } else if (*p == '[') {
            const char *q = p + 1;

            if (*q && glob_bracket(&q, (unsigned char)*s)) {
                p = q;
                ok = 1;
            }
        } else if (*p == *s) {
            p++;
            ok = 1;
        }
        if (ok) {
            s++;
            continue;
        }
        if (!star || !*back) return 0;
        p = star;
        s = ++back;
    }
}

#define GR_W  48                         /* the frame, 48 by 5: input, buttons */
#define GR_H  5
#define GR_FW 40                         /* the input's width */
static struct line gline;
static int  gfresh;                      /* the offered pattern is still whole: a key replaces it */
static char gpat[64];                    /* the last pattern: offered again; none at first */
static int  gsel;                        /* 1 to select, 0 to unselect */
static int  grx, gry;                    /* the frame's corner */

static void gr_geom(void)
{
    grx = (cols - GR_W) / 2;
    gry = (rows - GR_H) / 2 - 2;
    if (grx < 0) grx = 0;
    if (gry < 0) gry = 0;
}

/* The dialog, for the active panel: + selects, - unselects. */
static void gr_open(int sel)
{
    gsel = sel;
    line_clear(&gline);
    for (const char *c = gpat; *c; c++) line_key(&gline, (unsigned char)*c, GR_FW, 0);
    gfresh = 1;
    mode = MODE_GROUP;
}

/* Marks the entries of P whose names match the pattern: "..", never. */
static void gr_apply(struct panel *p)
{
    for (int i = 0; i < p->ne; i++) {
        struct ent *e = &p->e[i];

        if (!e->up && glob_match(gpat, p->nm + e->name)) e->sel = (uint8_t)gsel;
    }
}

/* * inverts the marks of the files of P: not the directories, not "..". */
static void gr_invert(struct panel *p)
{
    for (int i = 0; i < p->ne; i++) {
        struct ent *e = &p->e[i];

        if (e->up || (e->cls == CL_DIR && !e->link)) continue;
        e->sel ^= 1;
    }
}

static void gr_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return;
    }
    if (key == '\r' || key == '\n') {
        mode = MODE_NORMAL;
        if (!gline.len) return;
        strlcpy(gpat, gline.buf, sizeof gpat);
        gr_apply(&P[act]);
        return;
    }
    if (gfresh && key >= 0x20 && key < 0x7f) line_clear(&gline);
    gfresh = 0;
    line_key(&gline, key, GR_FW, 0);
}

static void gr_row(int y)
{
    int fl = grx, fr = grx + GR_W - 1, x, dy = y - gry;

    if (y < gry - DIALOG_PAD || y >= gry + GR_H + DIALOG_PAD) return;
    for (x = fl - DIALOG_PAD; x <= fr + DIALOG_PAD; x++) setc(x, ' ', C_QCD);
    if (dy < 0 || dy >= GR_H) return;
    if (dy == 0) {
        const char *t = gsel ? " Select " : " Unselect ";
        int tl = (int)strlen(t);

        setc(fl, 'l', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'k', C_QCD | DEC);
        sets(fl + 1 + (GR_W - 2 - tl) / 2, t, tl, C_QTITLE);
    } else if (dy == GR_H - 1) {
        setc(fl, 'm', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'j', C_QCD | DEC);
    } else if (dy == 2) {
        setc(fl, 't', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'u', C_QCD | DEC);
    } else {
        setc(fl, 'x', C_QCD | DEC);
        setc(fr, 'x', C_QCD | DEC);
        if (dy == 1) {
            line_view(&gline, GR_FW, 0);
            line_draw(&gline, fl + 2, GR_FW, C_FIELD);
        }
        if (dy == 3) {
            sd_item(fl + 14, "", "[< &OK >]", 1, 0);
            sd_item(fl + 23, "", "[ &Cancel ]", 0, 0);
        }
    }
}
