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
/* Output buffer                                                        */
/* ================================================================== */

typedef struct { char *data; size_t pos, cap; } SBuf;

static void sb_init(SBuf *b, size_t cap)
    { b->data = malloc(cap); b->pos = 0; b->cap = cap; }
static void sb_cat(SBuf *b, const char *s, size_t n)
    { if (b->pos + n < b->cap) { memcpy(b->data+b->pos, s, n); b->pos += n; } }
static void sb_str(SBuf *b, const char *s) { sb_cat(b, s, strlen(s)); }
static void sb_free(SBuf *b) { free(b->data); b->data = NULL; }

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

/* VT-to-VT emit: one combined SGR sequence per piece of text, then the
 * text itself. A style fully specifies both fg and bg, so each call is
 * self-contained -- no "reset to bar colour" bookkeeping needed between
 * pieces the way separate _FG()/_BG() macro pairs would have required. */
static void vt_emit(void *vctx, const char *s, int n, Style style)
{
    if (n <= 0) return;
    SBuf *b = vctx;
    char sgr[24];
    int m = snprintf(sgr, sizeof sgr, "\033[38;5;%u;48;5;%um",
                     style.fg, style.bg);
    sb_cat(b, sgr, (size_t)m);
    sb_cat(b, s, (size_t)n);
}

int status_render(char *_out, size_t _outlen,
                  int rows, int cols, int active,
                  pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
    size_t _need = (size_t)(cols * 8 + 512);
    SBuf b;
    sb_init(&b, _need);

    /* Move to last row, col 1.
     * Disable auto-wrap (DECAWM off) so writing into the very last cell
     * of the last row does not trigger a terminal scroll. */
    char move[48];
    snprintf(move, sizeof(move), "\033[%d;1H\033[?7l", rows);
    sb_str(&b, move);

    status_layout(vt_emit, &b, cols, active, child_pids, wins_exist, wins_alive);

    /* Reset colours and re-enable auto-wrap */
    sb_str(&b, _RESET "\033[?7h");

    int _ret = -1;
    if (b.pos <= _outlen) {
        memcpy(_out, b.data, b.pos);
        _ret = (int)b.pos;
    }
    sb_free(&b);
    return _ret;
}

void status_draw(int rows, int cols, int active,
                 pid_t child_pids[], bool wins_exist[], bool wins_alive[])
{
#ifdef X11_BACKEND
    x11_backend_status(rows - 1, cols, active, child_pids, wins_exist, wins_alive);
    return;
#endif
    char _buf[4096];
    int n = status_render(_buf, sizeof(_buf), rows, cols, active,
                          child_pids, wins_exist, wins_alive);
    if (n > 0) (void)write(STDOUT_FILENO, _buf, (size_t)n);
}
