/* sort.h - the order of a panel: how names and entries compare, and sorting the list. */

/* A character to compare, folded to lower case if FOLD. */
static int kc(unsigned char c, int fold) { return fold ? lc(c) : c; }

/* Strings compare bytewise, or without case if FOLD. */
static int fcmp(const char *a, const char *b, int fold)
{
    while (*a && kc(*a, fold) == kc(*b, fold)) { a++; b++; }
    return kc(*a, fold) - kc(*b, fold);
}

/* Names compare by the part before the first dot, then the part after it,
 * bytewise or without case (FOLD). Equivalent to sorting the first dot below
 * every other character, which is what mc's filename collation does (measured
 * against mc 4.8). */
static int ncmp(const char *a, const char *b, int fold)
{
    const char *da = a, *db = b;
    size_t la, lb, m;

    while (*da && *da != '.') da++;
    while (*db && *db != '.') db++;
    la = (size_t)(da - a);
    lb = (size_t)(db - b);
    m = la < lb ? la : lb;
    for (size_t i = 0; i < m; i++)
        if (kc(a[i], fold) != kc(b[i], fold)) return kc(a[i], fold) - kc(b[i], fold);
    if (la != lb) return la < lb ? -1 : 1;
    if (!*da && !*db) return 0;
    if (!*da) return -1;
    if (!*db) return 1;
    return fcmp(da + 1, db + 1, fold);
}

#if VERSION_SORT
/* Version order, as mc's filevercmp (gnulib's filenvercmp): digit runs compare as
 * numbers, letters sort before other characters, a tilde before everything, dot
 * names first, and the suffix of a name (.ext) after the rest of it. */
static int vdigit(int c) { return c >= '0' && c <= '9'; }
static int valpha(int c) { c |= 0x20; return c >= 'a' && c <= 'z'; }
static int valnum(int c) { return vdigit(c) || valpha(c); }

/* The length of the name without its suffix: the names compare without it first. */
static size_t vprefix(const char *s, size_t n)
{
    size_t i = 0, pl = 0;

    for (;;) {
        if (i == n) return pl;
        i++;
        pl = i;
        while (i + 1 < n && s[i] == '.' && (valpha(s[i + 1]) || s[i + 1] == '~'))
            for (i += 2; i < n && (valnum(s[i]) || s[i] == '~'); i++)
                ;
    }
}

/* The order of the character at S[P] (N bytes): -1 at the end, 0 for a digit, a
 * letter as it is, -2 for a tilde, and any other byte above the letters. */
static int vorder(const char *s, size_t p, size_t n)
{
    int c;

    if (p >= n) return -1;
    c = (unsigned char)s[p];
    if (vdigit(c)) return 0;
    if (valpha(c)) return c;
    if (c == '~') return -2;
    return c + 256;
}

static int verrevcmp(const char *s1, size_t n1, const char *s2, size_t n2)
{
    size_t p1 = 0, p2 = 0;

    while (p1 < n1 || p2 < n2) {
        int first = 0;

        while ((p1 < n1 && !vdigit(s1[p1])) || (p2 < n2 && !vdigit(s2[p2]))) {
            int c1 = vorder(s1, p1, n1), c2 = vorder(s2, p2, n2);
            if (c1 != c2) return c1 - c2;
            p1++;
            p2++;
        }
        while (p1 < n1 && s1[p1] == '0') p1++;
        while (p2 < n2 && s2[p2] == '0') p2++;
        while (p1 < n1 && p2 < n2 && vdigit(s1[p1]) && vdigit(s2[p2])) {
            if (!first) first = s1[p1] - s2[p2];
            p1++;
            p2++;
        }
        if (p1 < n1 && vdigit(s1[p1])) return 1;
        if (p2 < n2 && vdigit(s2[p2])) return -1;
        if (first) return first;
    }
    return 0;
}

static int filevercmp(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b), pa, pb;
    int r;

    if (!na || !nb) return !na ? (nb ? -1 : 0) : 1;
    if (a[0] == '.') {
        if (b[0] != '.') return -1;
        if (na == 1 || nb == 1) return na == 1 ? (nb == 1 ? 0 : -1) : 1;
        {
            int ad = a[1] == '.' && na == 2, bd = b[1] == '.' && nb == 2;
            if (ad || bd) return ad ? (bd ? 0 : -1) : 1;
        }
    } else if (b[0] == '.') {
        return 1;
    }
    pa = vprefix(a, na);
    pb = vprefix(b, nb);
    r = verrevcmp(a, pa, b, pb);
    if (r || (pa == na && pb == nb)) return r;
    return verrevcmp(a, na, b, nb);
}

#endif

/* The extension of a name, as mc's extension(): after the last dot, or "". */
static const char *ext_of(const char *s)
{
    const char *d = NULL;

    for (; *s; s++)
        if (*s == '.') d = s;
    return d ? d + 1 : "";
}

/* The group an entry sorts in, first: directories; then executables, if SO_EXEC. */
static int grp(const struct ent *e, int s)
{
    return is_dir(e) ? 2 : (s & SO_EXEC) && is_exe(e) ? 1 : 0;
}

static int kcmp(int64_t a, int64_t b) { return (a > b) - (a < b); }

/* Two names by the panel's flags: a dot name always comes first, and otherwise
 * the order is reversed if SO_REV. */
static int namecmp(const struct panel *p, uint16_t x, uint16_t y)
{
    const char *a = p->nm + p->e[x].name, *b = p->nm + p->e[y].name;
    int r;

    if ((a[0] == '.') != (b[0] == '.')) return a[0] == '.' ? -1 : 1;
    r = ncmp(a, b, p->sort & SO_FOLD);
    return p->sort & SO_REV ? -r : r;
}

/* Two entries by the panel's sort: the group first, then the field, reversed if
 * SO_REV, then the name, unless the field is the inode. KEY holds the time or the
 * inode of each entry for the atime, ctime and inode sorts; it is NULL when the
 * entries have none, and those are then alike. */
static int ecmp(const struct panel *p, const int64_t *key, uint16_t x, uint16_t y)
{
    const struct ent *a = &p->e[x], *b = &p->e[y];
    int s = p->sort, f = s & SO_FIELD, r = 0;

    if (grp(a, s) != grp(b, s)) return grp(b, s) - grp(a, s);
    switch (f) {
    case SF_NAME:    return namecmp(p, x, y);
#if VERSION_SORT
    case SF_VERSION: r = filevercmp(p->nm + a->name, p->nm + b->name); break;
#endif
    case SF_EXT:     r = fcmp(ext_of(p->nm + a->name), ext_of(p->nm + b->name), s & SO_FOLD); break;
    case SF_SIZE:    r = kcmp((int64_t)ent_size(a), (int64_t)ent_size(b)); break;
    case SF_MTIME:   r = kcmp(ent_mtime(a), ent_mtime(b)); break;
    case SF_ATIME:
    case SF_CTIME:
    case SF_INODE:   r = kcmp(key ? key[x] : 0, key ? key[y] : 0); break;
    default:         return 0;
    }
    if (r) return s & SO_REV ? -r : r;
    return f == SF_INODE ? 0 : namecmp(p, x, y);
}

/* The list: "..." first (not in the root directory, and always inside an archive),
 * then the rest in the panel's sort order, or as read if it is unsorted. */
static void sort_list(struct panel *p, const int64_t *key)
{
    int up = p->arc[0] || p->rem || !(p->path[0] == '/' && p->path[1] == 0), first = up ? 1 : 0;

    p->n = 0;
    if (up) p->ord[p->n++] = 0;
    for (int i = 1; i < p->ne; i++) p->ord[p->n++] = (uint16_t)i;
    if ((p->sort & SO_FIELD) == SF_UNSORTED) return;
    /* insertion sort; plenty fast for ordinary directories */
    for (int i = first + 1; i < p->n; i++) {
        uint16_t v = p->ord[i];
        int j = i;
        while (j > first && ecmp(p, key, p->ord[j - 1], v) > 0) {
            p->ord[j] = p->ord[j - 1];
            j--;
        }
        p->ord[j] = v;
    }
}
