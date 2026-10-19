#include "status.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef X11_BACKEND
#include "x11_backend.h"
#endif

/* ================================================================== */
/* Process name                                                         */
/* ================================================================== */

static void proc_name(pid_t pid, char *buf, size_t buflen)
{
    if (pid <= 0) { snprintf(buf, buflen, "?"); return; }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, buflen, "?"); return; }
    if (!fgets(buf, (int)buflen, f))
        snprintf(buf, buflen, "?");
    else {
        size_t l = strlen(buf);
        if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
    }
    fclose(f);
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void status_layout(StatusEmit emit, void *ctx, int cols, int active,
                   pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    /* ---- hostname (cached - won't change during a session) ---- */
    static char hostname[64] = "";
    if (hostname[0] == '\0') {
        if (gethostname(hostname, sizeof(hostname)) < 0)
            strcpy(hostname, "localhost");
        hostname[sizeof(hostname)-1] = '\0';
        char *dot = strchr(hostname, '.');
        if (dot) *dot = '\0';
    }

    /* ---- clock: HH:MM only ---- */
    char clock_str[8];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(clock_str, sizeof(clock_str), "%H:%M", tm);

    /* ---- tab labels ---- */
    char tab_text[MAX_WINDOWS][24];
    int  tab_w   [MAX_WINDOWS];

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins_exist[i]) { tab_text[i][0]='\0'; tab_w[i]=0; continue; }
        char pname[16] = "";
        if (wins_alive[i])
            proc_name(child_pids[i], pname, sizeof(pname));
        else
            snprintf(pname, sizeof(pname), "dead");
        if (strlen(pname) > 10) pname[10] = '\0';
        tab_w[i] = snprintf(tab_text[i], sizeof(tab_text[i]),
                            "%d:%s", i + 1, pname);
    }

    int host_w  = (int)strlen(hostname) + 2;
    int clock_w = (int)strlen(clock_str) + 2;

    /* Left: hostname */
    emit(ctx, " ", 1, STATUS_LEFT_COLOR);
    emit(ctx, hostname, (int)strlen(hostname), STATUS_LEFT_COLOR);
    int printed = host_w - 1;  /* no trailing space on hostname */

    /* Window tabs: separator space is always bar-bg; only the label
     * itself gets the tab highlight colour. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins_exist[i]) continue;
        emit(ctx, " ", 1, STATUS_BAR_COLOR);        /* unhighlighted gap */
        Style st = (i == active) ? WINDOW_STATUS_CURRENT_COLOR :
                   !wins_alive[i] ? WINDOW_STATUS_ACTIVITY_COLOR :
                                    WINDOW_STATUS_COLOR;
        emit(ctx, tab_text[i], tab_w[i], st);
        printed += tab_w[i] + 1;                    /* label + separator */
    }

    /* One space after the last tab before the padding region */
    emit(ctx, " ", 1, STATUS_BAR_COLOR);
    printed += 1;

    /* Padding, in chunks -- may be wider than any single fixed buffer */
    int pad = cols - printed - clock_w;
    static const char spaces[64] =
        "                                                                ";
    while (pad > 0) {
        int chunk = pad < (int)sizeof spaces ? pad : (int)sizeof spaces;
        emit(ctx, spaces, chunk, STATUS_BAR_COLOR);
        pad -= chunk;
    }

    /* Right: clock */
    emit(ctx, " ", 1, STATUS_RIGHT_COLOR);
    emit(ctx, clock_str, (int)strlen(clock_str), STATUS_RIGHT_COLOR);
    emit(ctx, " ", 1, STATUS_RIGHT_COLOR);
}

/* Cell emit: the text goes into a row of cells, in the style given.  Only
 * printable ASCII is kept; any other byte (a label is a program's own name,
 * which it chooses) shows as '?', so it can never reach the terminal as a
 * control sequence, and one byte is one column as the layout counts it. */
typedef struct { Cell *row; int cols, x; } CellFill;

int status_put(Cell *row, int cols, int x, const char *s, int n, Style st)
{
    for (int i = 0; i < n && x < cols; i++, x++) {
        unsigned char ch = (unsigned char)s[i];
        Cell *c = &row[x];
        memset(c, 0, sizeof *c);          /* flags 0: colours are explicit */
        c->ch = (ch >= 0x20 && ch < 0x7f) ? ch : '?';
        c->fg = st.fg;
        c->bg = st.bg;
    }
    return x;
}

static void cell_emit(void *ctx, const char *s, int n, Style st)
{
    CellFill *f = ctx;
    f->x = status_put(f->row, f->cols, f->x, s, n, st);
}

void status_cells(Cell *row, int cols, int active,
                  pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    CellFill f = { row, cols, 0 };
    status_layout(cell_emit, &f, cols, active, child_pids, wins_exist, wins_alive);
    for (; f.x < cols; f.x++) {           /* a layout that came out short */
        memset(&row[f.x], 0, sizeof row[f.x]);
        row[f.x].ch = ' ';
        row[f.x].fg = STATUS_BAR_FG;
        row[f.x].bg = STATUS_BAR_BG;
    }
}

#ifdef X11_BACKEND
void status_draw(int rows, int cols, int active,
                 pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    x11_backend_status(rows - 1, cols, active, child_pids, wins_exist, wins_alive);
}
#endif
