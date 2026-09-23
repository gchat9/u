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
/* Colour macros                                                        */
/* ================================================================== */

/* Colour constants: resolved from config.h style macros at compile time */
#define C_RESET     _RESET
#define C_BAR_BG    STATUS_BAR_STYLE
#define C_WIN_NORM  WINDOW_STATUS_STYLE
#define C_WIN_ACT   WINDOW_STATUS_CURRENT_STYLE
#define C_WIN_DEAD  WINDOW_STATUS_ACTIVITY_STYLE
#define C_HOST      STATUS_LEFT_STYLE
#define C_CLOCK     STATUS_RIGHT_STYLE

/* ================================================================== */
/* Output buffer                                                        */
/* ================================================================== */

typedef struct { char *data; size_t pos, cap; } SBuf;

static void sb_init(SBuf *b, size_t cap)
    { b->data = malloc(cap); b->pos = 0; b->cap = cap; }
static void sb_cat(SBuf *b, const char *s, size_t n)
    { if (b->pos + n < b->cap) { memcpy(b->data+b->pos, s, n); b->pos += n; } }
static void sb_str(SBuf *b, const char *s) { sb_cat(b, s, strlen(s)); }
static void sb_pad(SBuf *b, int n)
    { for (int i = 0; i < n; i++) sb_cat(b, " ", 1); }
static void sb_free(SBuf *b) { free(b->data); b->data = NULL; }

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

int status_render(char *_out, size_t _outlen,
                  int rows, int cols, int active,
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
    int  tabs_total = 0;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins_exist[i]) { tab_text[i][0]='\0'; tab_w[i]=0; continue; }
        char pname[16] = "";
        if (wins_alive[i])
            proc_name(child_pids[i], pname, sizeof(pname));
        else
            snprintf(pname, sizeof(pname), "dead");
        if (strlen(pname) > 10) pname[10] = '\0';
        int n = snprintf(tab_text[i], sizeof(tab_text[i]),
                         "%d:%s", i + 1, pname);
        tab_w[i]    = n;
        tabs_total += n + 1;
    }

    int host_w  = (int)strlen(hostname) + 2;
    int clock_w = (int)strlen(clock_str) + 2;

    /* ---- assemble ---- */
    size_t _need = (size_t)(cols * 8 + 512);
    SBuf b;
    sb_init(&b, _need);

    /* Move to last row, col 1.
     * Disable auto-wrap (DECAWM off) so writing into the very last cell
     * of the last row does not trigger a terminal scroll. */
    char move[48];
    snprintf(move, sizeof(move), "\033[%d;1H\033[?7l", rows);
    sb_str(&b, move);

    /* Left: hostname */
    sb_str(&b, C_BAR_BG);
    sb_str(&b, C_HOST);
    sb_cat(&b, " ", 1);
    sb_str(&b, hostname);
    sb_str(&b, C_BAR_BG);
    int printed = host_w - 1;  /* no trailing space on hostname */

    /* Window tabs: separator space is always bar-bg; only the label
     * itself gets the tab highlight colour. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins_exist[i]) continue;
        sb_str(&b, C_BAR_BG);
        sb_cat(&b, " ", 1);                         /* unhighlighted gap */
        if (i == active)         sb_str(&b, C_WIN_ACT);
        else if (!wins_alive[i]) sb_str(&b, C_WIN_DEAD);
        else                     sb_str(&b, C_WIN_NORM);
        sb_str(&b, tab_text[i]);
        sb_str(&b, C_BAR_BG);
        printed += tab_w[i] + 1;                    /* label + separator */
    }

    /* One space after the last tab before the padding region */
    sb_str(&b, C_BAR_BG);
    sb_cat(&b, " ", 1);
    printed += 1;

    /* Padding */
    int pad = cols - printed - clock_w;
    if (pad > 0) sb_pad(&b, pad);

    /* Right: clock */
    sb_str(&b, C_CLOCK);
    sb_cat(&b, " ", 1);
    sb_str(&b, clock_str);
    sb_cat(&b, " ", 1);

    /* Reset colours and re-enable auto-wrap */
    sb_str(&b, C_RESET "\033[?7h");

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
