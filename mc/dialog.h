/* dialog.h - the boxes that are drawn over the panels: the error box, the quick
 * cd box, and the question of a job (Yes/No, a failure, a directory). */

/* ------------------------------------------------------------ error box */

/* Open the error box: "Cannot change directory to", the path as typed, and
 * the system error with its number. */
static void set_error(const char *head, const char *shown, long e)
{
    char num[16];
    int w = 0, maxp = cols - 6, l;

    elen[0] = cat(eline[0], head);
    strlcpy(eline[1], shown, sizeof eline[1]);
    l = (int)strlen(eline[1]);
    elen[1] = cut_cols(eline[1], l, maxp);
    eline[1][elen[1]] = 0;
    elen[2] = cat(eline[2], errstr(e));
    eline[2][elen[2]++] = ' ';
    eline[2][elen[2]++] = '(';
    l = (int)utoa((unsigned long)e, num);
    for (int i = 0; i < l; i++) eline[2][elen[2]++] = num[i];
    eline[2][elen[2]++] = ')';
    eline[2][elen[2]] = 0;
    for (int i = 0; i < 3; i++) if (tcols(eline[i], elen[i]) > w) w = tcols(eline[i], elen[i]);
    ewidth = w + 4 + 2 * DIALOG_PAD;
    if (ewidth > cols) ewidth = cols;
    mode = MODE_ERR;
}

/* Position of the error box: mc's wtools placement for message dialogs. */
static void err_geom(int *x, int *y)
{
    int h = ERR_H, w = ewidth;
    int ypos = rows / 3 - (h - 3) / 2;

    if (ypos + h < rows / 2) {                     /* too high: centre it */
        *y = (rows - h) / 2;
        *x = (cols - w) / 2;
    } else {
        *y = ypos;
        *x = cols / 2 - w / 2;
    }
    if (*y < 0) *y = 0;
    if (*x < 0) *x = 0;
}

static void err_draw_row(int y, int ex, int ey)
{
    int dy = y - ey - DIALOG_PAD, fw = ewidth - 2 * DIALOG_PAD;
    int x0 = ex + DIALOG_PAD, r = x0 + fw - 1, x, i;

    if (y < ey || y >= ey + ERR_H) return;
    for (x = ex; x < ex + ewidth; x++) setc(x, ' ', C_ERR);
    if (dy < 0 || dy >= 7) return;
    if (dy == 0) {                                 /* top frame with title */
        setc(x0, 'l', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'k', C_ERR | DEC);
        sets(x0 + 2 + (fw - 4 - 7) / 2, " Error ", 7, C_ERRHEAD);
    } else if (dy == 6) {                          /* bottom frame */
        setc(x0, 'm', C_ERR | DEC);
        for (x = x0 + 1; x < r; x++) setc(x, 'q', C_ERR | DEC);
        setc(r, 'j', C_ERR | DEC);
    } else {                                       /* sides; the message lines, centred */
        setc(x0, 'x', C_ERR | DEC);
        setc(r, 'x', C_ERR | DEC);
        if (dy >= 2 && dy <= 4) {
            i = dy - 2;
            sets(x0 + 2 + (fw - 4 - tcols(eline[i], elen[i])) / 2, eline[i], elen[i], C_ERR);
        }
    }
}

/* Quick cd box: five rows, starting at row rows-7 (one row above the active
 * panel's bottom border), as wide as the panel. Row Y of the box, for the panel
 * at X0 with width PW. The top and bottom rows are blank margins. */
static void qcd_draw_row(int y, int x0, int pw)
{
    int top = rows - 2 - QCD_H, dy = y - top, r = x0 + pw - 1;
    int fl = x0 + DIALOG_PAD, fr = r - DIALOG_PAD, fd, x;
    const char *title = " Quick cd ";

    if (y < top || y >= top + QCD_H) return;
    for (x = x0; x <= r; x++) setc(x, ' ', C_QCD);
    if (dy < DIALOG_PAD || dy >= QCD_H - DIALOG_PAD) return;       /* the margin */
    fd = dy - DIALOG_PAD;
    if (fd == 0 || fd == 2) {                      /* frame, titled on top */
        setc(fl, fd == 0 ? 'l' : 'm', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, fd == 0 ? 'k' : 'j', C_QCD | DEC);
        if (fd == 0) sets(fl + 1 + (fr - fl - 1 - 10) / 2, title, 10, C_QTITLE);
    } else {                                       /* input line */
        int f = (pw - 2 * DIALOG_PAD) - 10;

        setc(fl, 'x', C_QCD | DEC);
        setc(fr, 'x', C_QCD | DEC);
        sets(fl + 2, "cd", 2, C_QCD);
        line_view(&qline, f + 3, 3);
        line_draw(&qline, fl + 5, f, C_FIELD);
        sets(fl + 5 + f, "[^]", 3, C_HIST);
    }
}

/* The dialogs of a file operation, as mc shows them: a box in the middle of the
 * screen with a title, two lines of text and buttons, one of them selected.  The
 * question is Yes or No; a failure is Skip, Skip all, Retry or Abort.  Only
 * files are deleted: F8 on a directory does nothing. */
enum { QY_ASK, QY_ERR, QY_DIR };
struct qy_button { const char *label; int hot; };   /* hot: the index of the hotkey in the label */
static const struct qy_button qy_yesno[2] = { { "[ Yes ]", 2 }, { "[ No ]", 2 } };
static const struct qy_button qy_recurse[5] = { { "[ Yes ]", 2 }, { "[ No ]", 2 }, { "[ All ]", 3 },
                                                 { "[ None ]", 5 }, { "[ Abort ]", 2 } };
static const struct qy_button qy_failed[4] = { { "[ Skip ]", 2 }, { "[ Skip all ]", 5 },
                                               { "[ Retry ]", 2 }, { "[ Abort ]", 2 } };
static int  qy_kind;
static char qy_t1[320], qy_t2[320];     /* the two lines of text */
static int  qy_n1, qy_n2;
static int  qy_sel;                     /* the selected button */
static int  qrx, qry, qrw, qrh;         /* the box, with its margin of one cell */

static const struct qy_button *qy_buttons(int *n)
{
    *n = qy_kind == QY_ASK ? 2 : qy_kind == QY_DIR ? 5 : 4;
    return qy_kind == QY_ASK ? qy_yesno : qy_kind == QY_DIR ? qy_recurse : qy_failed;
}

/* The box, and where it goes.  Its width is that of the widest of the title, the
 * text and the buttons, each button with two cells more than its label.  Where it
 * goes is as mc puts it: the question is placed as a box of six lines, in the upper
 * third of the screen; a failure is placed as a box of four lines, centred, and
 * then grows downwards to its eight. */
static void qy_geom(void)
{
    int nb, win = 0, m, t1 = tcols(qy_t1, qy_n1), t2 = tcols(qy_t2, qy_n2);
    const struct qy_button *b = qy_buttons(&nb);

    for (int i = 0; i < nb; i++) win += (int)strlen(b[i].label) + 2;
    m = win;
    if (6 > m) m = 6;                   /* the title: "Delete", 5 for "Error" */
    if (t1 > m) m = t1;
    if (t2 > m) m = t2;
    qrw = m + 4 + 2 * DIALOG_PAD;
    qrh = 6 + 2 * DIALOG_PAD;          /* two lines of text, and the frame, with the margin */
    qrx = (cols - qrw + 1) / 2;
    qry = qy_kind != QY_ERR ? (rows - 6) / 3 : (rows - 4) / 2;
    if (qrx < 0) qrx = 0;
    if (qry < 0) qry = 0;
}

/* One button's label, in its place: the hotkey in brown. */
static void qy_label(int x, const char *label, int hot, uint16_t base, uint16_t key)
{
    for (int k = 0; label[k]; k++)
        setc(x + k, (unsigned char)label[k], k == hot ? key : base);
}

static void qy_draw_row(int y)
{
    int fx = qrx + DIALOG_PAD, fw = qrw - 2 * DIALOG_PAD, fh = 6, in = fw - 2;
    int dy = y - (qry + DIALOG_PAD), x, nb;
    const struct qy_button *b;

    if (y < qry || y >= qry + qrh) return;
    for (x = qrx; x < qrx + qrw; x++) setc(x, ' ', C_ERR);
    if (dy < 0 || dy >= fh) return;
    setc(fx, dy == 0 ? 'l' : dy == 5 ? 'm' : dy == 3 ? 't' : 'x', C_ERR | DEC);
    setc(fx + fw - 1, dy == 0 ? 'k' : dy == 5 ? 'j' : dy == 3 ? 'u' : 'x', C_ERR | DEC);
    if (dy == 0 || dy == 3 || dy == 5)
        for (x = fx + 1; x < fx + fw - 1; x++) setc(x, 'q', C_ERR | DEC);
    if (dy == 0) {
        const char *title = qy_kind != QY_ERR ? " Delete " : " Error ";
        int tl = (int)strlen(title);

        sets(fx + 1 + (in - tl) / 2, title, tl, C_ERRHEAD);
    }
    if (dy == 1) sets(fx + 1 + (in - tcols(qy_t1, qy_n1)) / 2, qy_t1, qy_n1, C_ERR);
    if (dy == 2) sets(fx + 1 + (in - tcols(qy_t2, qy_n2)) / 2, qy_t2, qy_n2, C_ERR);
    if (dy == 4) {
        int block = 0, at;

        b = qy_buttons(&nb);
        for (int i = 0; i < nb; i++) block += (int)strlen(b[i].label) + (i ? 2 : 0);
        at = fx + 1 + (in - block) / 2;
        for (int i = 0; i < nb; i++) {
            int sel = qy_sel == i;
            uint16_t base = sel ? C_TITLE : C_ERR;
            uint16_t key = sel ? ATTR(BROWN, WHITE, 1) : C_ERRHEAD;

            qy_label(at, b[i].label, b[i].hot, base, key);
            at += (int)strlen(b[i].label) + 2;
        }
    }
}
