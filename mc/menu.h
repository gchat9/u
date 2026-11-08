/* ----------------------------------------------------------- menu bar */

/* The menu bar, up on F9 or ESC 9: mbt is the title it is on, and md whether that
 * title's dropdown is down. The Left and Right menus hold the one entry, Sort
 * order...; File, Command and Options are empty for now. Left and Right are the
 * panels on the screen, not the active one: F9 starts on the side of the active
 * panel. */
enum { MT_LEFT, MT_FILE, MT_CMD, MT_OPT, MT_RIGHT, MT_N };
static const char *const mt_text[MT_N] = { "&Left", "&File", "&Command", "&Options", "&Right" };
static const char *const mt_item[] = { "&Sort order...", "S&hell link..." };
static int mbt, md;
static int mdx, mdw, mdh;                /* the dropdown's box, from md_geom() */

static int mcur;                         /* the entry the dropdown highlights */

static int menu_n(int t) { return t == MT_LEFT || t == MT_RIGHT ? 2 : 0; }

/* The letter of a label's hotkey (the one after the &), in lower case, which is
 * the key that picks it; 0 if none. */
static int hot_key(const char *s)
{
    for (; *s; s++)
        if (*s == '&') return lc((unsigned char)s[1]);
    return 0;
}

/* The cells a label takes: its text, without the &. */
static int hot_len(const char *s)
{
    int n = 0;

    for (; *s; s++)
        if (*s != '&') n++;
    return n;
}

/* A label from X: its hotkey letter in colour h, the rest in colour a. */
static void hot_put(int x, const char *s, uint16_t a, uint16_t h)
{
    int hot = 0;

    for (; *s; s++) {
        if (*s == '&') { hot = 1; continue; }
        setc(x++, (unsigned char)*s, hot ? h : a);
        hot = 0;
    }
}

/* Where title T starts on the bar, as mc arranges them: each takes its text with a
 * space either side, and the gaps between the titles are as wide as fit, from 1
 * to 3. */
static int mt_x(int t)
{
    int gap = cols - 2, x = 1;

    for (int i = 0; i < MT_N; i++) gap -= hot_len(mt_text[i]) + 2;
    gap /= MT_N - 1;
    if (gap <= 0) gap = 1;
    if (gap > 3) gap = 3;
    for (int i = 0; i < t; i++) x += hot_len(mt_text[i]) + 2 + gap;
    return x;
}

/* The bar, row 0: the titles, the one the menu is on in the selected colours. */
static void menu_bar(void)
{
    for (int x = 0; x < cols; x++) setc(x, ' ', C_MENU);
    for (int t = 0; t < MT_N; t++) {
        int x0 = mt_x(t), w = hot_len(mt_text[t]) + 2;
        uint16_t a = t == mbt ? C_MENUSEL : C_MENU;

        for (int x = x0; x < x0 + w; x++) setc(x, ' ', a);
        hot_put(x0 + 1, mt_text[t], a, t == mbt ? C_MENUSELHOT : C_MENUHOT);
    }
}

/* The dropdown of the title: a box under the title, its frame two cells left of
 * the title's text, as mc puts it; moved in if it would run past the edge. */
static void md_geom(void)
{
    int hot = 0;

    for (int i = 0; i < menu_n(mbt); i++)
        if (hot_len(mt_item[i]) > hot) hot = hot_len(mt_item[i]);
    mdw = hot + 3;                       /* the frame, a space, the text */
    mdh = menu_n(mbt) + 2;
    mdx = mt_x(mbt) - 1;
    if (mdx + mdw > cols) mdx = cols - mdw;
    if (mdx < 0) mdx = 0;
}

/* One row of the dropdown, row y (the top frame is row 1). The entry is the
 * highlighted one, as it is when the dropdown is opened. */
static void md_row(int y)
{
    int dy = y - 1, r = mdx + mdw - 1;

    if (dy < 0 || dy >= mdh) return;
    if (dy == 0) {
        setc(mdx, 'l', C_MENU | DEC);
        for (int x = mdx + 1; x < r; x++) setc(x, 'q', C_MENU | DEC);
        setc(r, 'k', C_MENU | DEC);
    } else if (dy == mdh - 1) {
        setc(mdx, 'm', C_MENU | DEC);
        for (int x = mdx + 1; x < r; x++) setc(x, 'q', C_MENU | DEC);
        setc(r, 'j', C_MENU | DEC);
    } else {
        for (int x = mdx; x <= r; x++) setc(x, ' ', C_MENU);
        setc(mdx, 'x', C_MENU | DEC);
        setc(r, 'x', C_MENU | DEC);
        {
            int sel = dy - 1 == mcur;

            for (int x = mdx + 1; x < r; x++) setc(x, ' ', sel ? C_MENUSEL : C_MENU);
            hot_put(mdx + 2, mt_item[dy - 1], sel ? C_MENUSEL : C_MENU, sel ? C_MENUSELHOT : C_MENUHOT);
        }
    }
}

/* The menu's entry, Sort order...: the sort of the panel on the side of the
 * title (Left is the left panel, Right the right one). */
static void menu_run(int item);
static void lk_open(int pi);

/* F9 or ESC 9 with no dialog open: the bar, on the side of the active panel. */
static void menu_open(void)
{
    mode = MODE_MENU;
    mbt = act == lft ? MT_LEFT : MT_RIGHT;
    md = 0;
    mcur = 0;
}

/* Keys on the bar. Left and Right change the title, Up and Down and Enter open
 * the dropdown, and Enter or the entry's letter (s) choose its entry. A title's
 * letter goes to that title and opens its dropdown. Esc takes the menu away; F9
 * does nothing. */
static int menu_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return 0;
    }
    if (key == K_F9 || key == (K_ALT | '9')) return 0;
    if (key == K_LEFT || key == K_RIGHT) {
        mbt = key == K_LEFT ? (mbt + MT_N - 1) % MT_N : (mbt + 1) % MT_N;
        mcur = 0;
        return 0;
    }
    if (key == K_UP || key == K_DOWN) {         /* opens the dropdown; then moves its highlight */
        int n = menu_n(mbt);

        if (!n) return 0;
        if (!md) md = 1;
        else mcur = key == K_UP ? (mcur + n - 1) % n : (mcur + 1) % n;
        return 0;
    }
    if (key == '\r' || key == '\n') {
        if (!menu_n(mbt)) return 0;
        if (!md) md = 1;
        else menu_run(mcur);
        return 0;
    }
    if (md)
        for (int i = 0; i < menu_n(mbt); i++)
            if (key == hot_key(mt_item[i])) {   /* an entry's letter chooses it */
                mcur = i;
                menu_run(i);
                return 0;
            }
    for (int t = 0; t < MT_N; t++)
        if (key == hot_key(mt_text[t])) {       /* the title's letter opens its dropdown */
            mbt = t;
            mcur = 0;
            md = 1;
            return 0;
        }
    return 0;
}

/* ----------------------------------------------------------- sort order */

/* The Sort order dialog, from the menu's entry: it is for the panel sd_p, and OK
 * sets its sort. Its focus is one of the widgets in Tab's order: the group of
 * fields (sd_fr is the field with the focus, sd_sel the one chosen), the three
 * checkboxes, OK, Cancel. */
enum { SD_RADIO, SD_EXEC, SD_CASE, SD_REV, SD_OK, SD_CANCEL, SD_N };
#define SD_W  45                         /* the frame, 45 by 13 */
#define SD_H  (SD_NF + 4)                /* the choices, and the rows round them */
#define SD_RW 15                         /* a field's highlight: the widest, with its mark */
static const struct { const char *text; int f; } sd_field[] = {
    { "&Unsorted", SF_UNSORTED }, { "&Name", SF_NAME },
#if VERSION_SORT
    { "&Version", SF_VERSION },
#endif
    { "E&xtension", SF_EXT }, { "&Size", SF_SIZE }, { "&Modify time", SF_MTIME },
    { "&Access time", SF_ATIME }, { "C&hange time", SF_CTIME }, { "&Inode", SF_INODE },
};
#if VERSION_SORT
#define SD_NF 9                          /* the choices of fields */
#else
#define SD_NF 8
#endif
static const char *const sd_box[3] = { "Executable &first", "Cas&e sensitive", "&Reverse" };
static const int sd_bit[3] = { SO_EXEC, SO_FOLD, SO_REV };   /* the flag a checkbox sets */
static int sd_p;                         /* the panel */
static int sd_w, sd_fr, sd_sel;          /* the focus, the field with it, the chosen field */
static int sd_flags;                     /* the SO_ flags, as they are being set */
static int sdx, sdy;                     /* the frame's corner, from sd_geom() */

/* A checkbox is on when its flag is; Case sensitive is on when SO_FOLD is off. */
static int sd_on(int k) { return ((sd_flags & sd_bit[k]) != 0) ^ (k == 1); }

static void sd_geom(void)
{
    sdx = (cols - SD_W) / 2;
    sdy = (rows - SD_H) / 3;
    if (sdx < 0) sdx = 0;
    if (sdy < 0) sdy = 0;
}

/* Open the dialog for panel PI, with its sort as the choice. */
static void sd_open(int pi)
{
    sd_p = pi;
    sd_flags = P[pi].sort & ~SO_FIELD;
    for (sd_sel = 0; sd_sel < SD_NF - 1 && sd_field[sd_sel].f != (P[pi].sort & SO_FIELD); sd_sel++)
        ;
    sd_fr = sd_sel;
    sd_w = SD_RADIO;
    mode = MODE_SORT;
    md = 0;
}

/* An entry of the dropdown: Sort order... for the panel on the side of the title, or
 * the Shell link, which is for that panel too. */
static void menu_run(int item)
{
    int pi = mbt == MT_LEFT ? lft : lft ^ 1;

    if (item == 0) sd_open(pi);
    else lk_open(pi);
}

/* OK sets the panel's sort, and reads the directory in it again, the highlight
 * staying on its file. Cancel changes nothing. */
static void sd_apply(void)
{
    P[sd_p].sort = (uint8_t)(sd_field[sd_sel].f | sd_flags);
    mode = MODE_NORMAL;
    panel_rescan(&P[sd_p]);
}

/* One choice: its prefix and label, drawn with the focus colours if FOC; the
 * highlight is W cells wide, or the text's width if 0. */
static void sd_item(int x, const char *pre, const char *label, int foc, int w)
{
    uint16_t a = foc ? C_FIELD : C_QCD, h = foc ? C_DHOTF : C_DHOT;
    int tw = (int)strlen(pre) + hot_len(label);

    if (w < tw) w = tw;
    for (int i = 0; i < w; i++) setc(x + i, ' ', a);
    for (int i = 0; pre[i]; i++) setc(x + i, (unsigned char)pre[i], a);
    hot_put(x + (int)strlen(pre), label, a, h);
}

/* Keys in the dialog. Up and Down go through the fields of the group, then leave
 * it; Tab, Right and Down go on to the next widget, Shift+Tab, Left and Up back to
 * the one before. Space chooses a field or toggles a checkbox, Enter is OK (Cancel
 * if it has the focus), Esc cancels, and each letter of a label picks its choice. */
static int sd_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return 0;
    }
    if (key == '\r' || key == '\n') {
        if (sd_w == SD_CANCEL) mode = MODE_NORMAL;
        else sd_apply();
        return 0;
    }
    if (key == K_DOWN && sd_w == SD_RADIO && sd_fr < SD_NF - 1) { sd_fr++; return 0; }
    if (key == K_UP && sd_w == SD_RADIO && sd_fr > 0) { sd_fr--; return 0; }
    if (key == 9 || key == K_RIGHT || key == K_DOWN) sd_w = (sd_w + 1) % SD_N;
    else if (key == K_BTAB || key == K_LEFT || key == K_UP) sd_w = (sd_w + SD_N - 1) % SD_N;
    else if (key == ' ') {
        if (sd_w == SD_RADIO) sd_sel = sd_fr;
        else if (sd_w == SD_OK) sd_apply();
        else if (sd_w == SD_CANCEL) mode = MODE_NORMAL;
        else sd_flags ^= sd_bit[sd_w - SD_EXEC];
    } else {
        for (int i = 0; i < SD_NF; i++)
            if (key == hot_key(sd_field[i].text)) {
                sd_w = SD_RADIO;
                sd_fr = sd_sel = i;
                return 0;
            }
        for (int k = 0; k < 3; k++)
            if (key == hot_key(sd_box[k])) {
                sd_w = SD_EXEC + k;
                sd_flags ^= sd_bit[k];
                return 0;
            }
        if (key == hot_key("[< &OK >]")) sd_apply();
        else if (key == hot_key("[ &Cancel ]")) mode = MODE_NORMAL;
    }
    return 0;
}

/* One row of the dialog, row y: its frame and margin, and the choices. */
static void sd_row(int y)
{
    int dy = y - sdy, r = sdx + SD_W - 1, x;

    if (y < sdy - DIALOG_PAD || y >= sdy + SD_H + DIALOG_PAD) return;
    for (x = sdx - DIALOG_PAD; x <= r + DIALOG_PAD; x++) setc(x, ' ', C_QCD);
    if (dy < 0 || dy >= SD_H) return;
    if (dy == 0) {
        setc(sdx, 'l', C_QCD | DEC);
        for (x = sdx + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        setc(r, 'k', C_QCD | DEC);
        sets(sdx + 1 + (SD_W - 2 - 12) / 2, " Sort order ", 12, C_QTITLE);
    } else if (dy == SD_H - 1) {
        setc(sdx, 'm', C_QCD | DEC);
        for (x = sdx + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        setc(r, 'j', C_QCD | DEC);
    } else if (dy == SD_NF + 1) {
        setc(sdx, 't', C_QCD | DEC);
        for (x = sdx + 1; x < r; x++) setc(x, 'q', C_QCD | DEC);
        setc(r, 'u', C_QCD | DEC);
    } else {
        setc(sdx, 'x', C_QCD | DEC);
        setc(r, 'x', C_QCD | DEC);
        if (dy >= 1 && dy <= SD_NF)
            sd_item(sdx + 2, sd_sel == dy - 1 ? "(*) " : "( ) ", sd_field[dy - 1].text,
                    sd_w == SD_RADIO && sd_fr == dy - 1, SD_RW);
        if (dy >= 1 && dy <= 3)
            sd_item(sdx + 23, sd_on(dy - 1) ? "[x] " : "[ ] ", sd_box[dy - 1],
                    sd_w == SD_EXEC + dy - 1, 0);
        if (dy == SD_NF + 2) {
            sd_item(sdx + 13, "", "[< &OK >]", sd_w == SD_OK, 0);
            sd_item(sdx + 22, "", "[ &Cancel ]", sd_w == SD_CANCEL, 0);
        }
    }
}

/* ----------------------------------------------------------- shell link */

/* The Shell link dialog, from the menu's entry: its input is user@host/dir, and OK
 * opens that link in panel LK_P (the link's own sh:// is put in front). Tab takes the
 * focus between the input and the buttons, Esc cancels. */
#define LK_W  48                         /* the frame, 48 by 6 */
#define LK_H  6
#define LK_FW 41                         /* the input's width, the [^] after it */
static struct line lline;
static int lk_p, lk_f;                   /* the panel, the focus: the input, OK, Cancel */
static int lkx, lky;                     /* the frame's corner, from lk_geom() */

static void lk_geom(void)
{
    lkx = (cols - LK_W) / 2;
    lky = (rows - LK_H) / 2 - 2;
    if (lkx < 0) lkx = 0;
    if (lky < 0) lky = 0;
}

static void lk_open(int pi)
{
    lk_p = pi;
    lk_f = 0;
    line_clear(&lline);
    mode = MODE_LINK;
    md = 0;
}

static void lk_apply(void)
{
    char url[320];

    mode = MODE_NORMAL;
    if (!lline.len) return;
    strlcpy(url, "sh://", sizeof url);
    strlcat(url, lline.buf, sizeof url);
    change_to(&P[lk_p], url, url);
}

static void lk_handle(int key)
{
    if (key == K_ESC) {
        mode = MODE_NORMAL;
        return;
    }
    if (key == 9 || key == K_BTAB) {
        lk_f = key == 9 ? (lk_f + 1) % 3 : (lk_f + 2) % 3;
        return;
    }
    if (key == '\r' || key == '\n') {
        if (lk_f == 2) mode = MODE_NORMAL;
        else lk_apply();
        return;
    }
    if (lk_f == 0) line_key(&lline, key, LK_FW, 3);
}

static void lk_row(int y)
{
    int fl = lkx, fr = lkx + LK_W - 1, x, dy = y - lky;

    if (y < lky - DIALOG_PAD || y >= lky + LK_H + DIALOG_PAD) return;
    for (x = fl - DIALOG_PAD; x <= fr + DIALOG_PAD; x++) setc(x, ' ', C_QCD);
    if (dy < 0 || dy >= LK_H) return;
    if (dy == 0) {
        setc(fl, 'l', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'k', C_QCD | DEC);
        sets(fl + 1 + (LK_W - 2 - 23) / 2, " Shell link to machine ", 23, C_QTITLE);
    } else if (dy == 5) {
        setc(fl, 'm', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'j', C_QCD | DEC);
    } else if (dy == 3) {
        setc(fl, 't', C_QCD | DEC);
        for (x = fl + 1; x < fr; x++) setc(x, 'q', C_QCD | DEC);
        setc(fr, 'u', C_QCD | DEC);
    } else {
        setc(fl, 'x', C_QCD | DEC);
        setc(fr, 'x', C_QCD | DEC);
        if (dy == 1) sets(fl + 2, "Enter machine name (F1 for details):", 36, C_QCD);
        if (dy == 2) {
            line_view(&lline, LK_FW + 3, 3);
            line_draw(&lline, fl + 2, LK_FW, C_FIELD);
            sets(fl + 2 + LK_FW, "[^]", 3, C_HIST);
        }
        if (dy == 4) {
            sd_item(fl + 14, "", "[< &OK >]", lk_f == 1, 0);
            sd_item(fl + 23, "", "[ &Cancel ]", lk_f == 2, 0);
        }
    }
}
