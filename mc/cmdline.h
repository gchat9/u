/* cmdline.h - the command line at the bottom of the screen: its prompt (PS1). */

/* Bounded append of S to OUT, which holds MAX bytes in all. */
static void addn(char *out, int *n, int max, const char *s)
{
    while (*s && *n < max - 2) out[(*n)++] = *s++;
}

static int idchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Expand PS1 (or mc's default, the usual user@host:path$ prompt) into OUT.
 * Handles \u \h \H \w \W \$ \n \\, drops \[ \] and colour escapes, and
 * expands $NAME and ${NAME} from the environment. */
static int ps1_expand(char *out, int max)
{
    static const char def[] = "\\u@\\h:\\w\\$ ";
    const char *ps = genv("PS1"), *s;
    char tmp[4200];
    int n = 0;

    if (!ps) ps = def;
    for (s = ps; *s && n < max - 2; s++) {
        if (*s == '$' && (s[1] == '{' || idchar(s[1])) && !(s[1] >= '0' && s[1] <= '9')) {
            char name[64];
            const char *b = s + 1, *e, *v;
            int k = 0;

            if (*b == '{') {
                b++;
                for (e = b; *e && *e != '}'; e++) ;
            } else {
                for (e = b; idchar(*e); e++) ;
            }
            while (b < e && k < 63) name[k++] = *b++;
            name[k] = 0;
            v = genv(name);
            addn(out, &n, max, v ? v : "");
            s = (*e == '}' && s[1] == '{') ? e : e - 1;
            continue;
        }
        if (*s != '\\' || !s[1]) {
            out[n++] = *s;
            continue;
        }
        s++;
        switch (*s) {
        case 'u': addn(out, &n, max, user); break;
        case 'h': {
            char h[65];
            int k = 0;

            while (host[k] && host[k] != '.' && k < 64) { h[k] = host[k]; k++; }
            h[k] = 0;
            addn(out, &n, max, h);
            break;
        }
        case 'H': addn(out, &n, max, host); break;
        case 'w': disp_path(tmp, sizeof tmp, P[act].path); addn(out, &n, max, tmp); break;
        case 'W': {
            const char *base = P[act].path;

            for (const char *q = P[act].path; *q; q++)
                if (*q == '/' && q[1]) base = q + 1;
            addn(out, &n, max, base);
            break;
        }
        case '$': out[n++] = uid == 0 ? '#' : '$'; break;
        case 'n': out[n++] = ' '; break;
        case '\\': out[n++] = '\\'; break;
        case '[': case ']': break;
        case 'e':                                  /* skip an ESC [ ... m colour */
            if (s[1] == '[') {
                while (s[1] && s[1] != 'm') s++;
                if (s[1]) s++;
            }
            break;
        default:
            out[n++] = '\\';
            out[n++] = *s;
            break;
        }
    }
    out[n] = 0;
    return n;
}

/* Columns for the command line text: after the prompt, and before the [^]
 * (three columns at the right, one blank column before them) once a command
 * has been run. */
static int cmd_width(void)
{
    int w = cols - 1 - prompt_len - (cmd_hist ? 4 : 0);

    return w < 1 ? 1 : w;
}

/* The command line: the prompt, then the text typed after it. */
static void prompt_row(void)
{
    char t[4200];
    int n = ps1_expand(t, 4000), w;

    n = cut_cols(t, n, cols / 2);
    prompt_len = tcols(t, n);
    sets(0, t, n, DEFA);
    w = cmd_width();
    line_view(&cmdl, w, 0);
    line_draw(&cmdl, prompt_len, w, DEFA);
    if (cmd_hist && cols >= prompt_len + 8) sets(cols - 3, "[^]", 3, C_HIST);
}

/* The prompt, as the shell would print it, to the terminal. */
static void put_prompt(void)
{
    char t[4200];
    int n = ps1_expand(t, 4000);

    put(t, n);
}
