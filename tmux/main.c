#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <locale.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#include "pty.h"
#include "vt.h"
#include "render.h"
#include "input.h"
#include "status.h"
#include "config.h"
#include "window.h"
#include "session.h"
#ifdef X11_BACKEND
#include "x11_backend.h"
#endif

static Window     *g_wins[MAX_WINDOWS];
static int         g_cur;
static RenderState g_rs;
static int         g_rows, g_cols;

static volatile sig_atomic_t g_winch = 0;
static volatile sig_atomic_t g_child = 0;
static volatile sig_atomic_t g_quit  = 0;
static volatile sig_atomic_t g_sighup = 0;

/* Listening socket fd; held for the lifetime of the session so that
 * attach works after an SSH disconnect even without explicit detach. */
static int g_session_sock = -1;

/* Observer connection fd; -1 when no observer is connected. */
static int g_observer_fd  = -1;

/* Scrollback mode: reading history rather than live screen */
static bool g_scrollback_mode   = false;
static int  g_scrollback_offset = 0;  /* lines scrolled back from most-recent */

/* Set by handle_session_conn on takeover. */
static int g_become_observer_rows = 0;
static int g_become_observer_cols = 0;
static int g_pending_obs_fd       = -1;

/* ================================================================== */
/* Signal handlers                                                      */
/* ================================================================== */

static void on_sigwinch(int sig) { (void)sig; g_winch  = 1; }
static void on_sigchld (int sig) { (void)sig; g_child  = 1; }
static void on_sigterm (int sig) { (void)sig; g_quit   = 1; }
static void on_sighup  (int sig) { (void)sig; g_sighup = 1; }

/* ================================================================== */
/* Timing                                                               */
/* ================================================================== */

#ifndef __linux__
/* On Linux we use select()'s timeval update to track elapsed time
 * without any clock calls.  On other platforms we fall back to
 * clock_gettime(). */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
#endif

/*
 * Milliseconds until just after the next whole minute.
 * The +100ms fudge ensures we land just past the boundary.
 */
static long ms_to_next_minute(void)
{
    time_t t       = time(NULL);
    long secs_left = 60L - (long)(t % 60);
    return secs_left * 1000L + 100L;
}

/* ================================================================== */
/* Terminal geometry                                                    */
/* ================================================================== */

static void get_term_size(int *rows, int *cols)
{
#ifdef X11_BACKEND
    /* The "terminal" is our own X window; whatever tty launched us is
     * irrelevant (and may not exist, or be a different size). */
    *rows = x11_backend_rows();
    *cols = x11_backend_columns();
    return;
#endif
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
            && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* Children see one less row: the bottom row belongs to the status bar */
static int child_rows(void) { return g_rows > 1 ? g_rows - 1 : 1; }

/* ================================================================== */
/* Status bar                                                           */
/* ================================================================== */

#if SCROLLBACK_ENABLED
static void render_scrollback_view(void);
#endif

static void redraw_status(void)
{
#if SCROLLBACK_ENABLED
    /* The scrollback viewer owns the status row (its footer), so a
     * redraw -- the minute tick, say -- repaints the viewer instead. */
    if (g_scrollback_mode) { render_scrollback_view(); return; }
#endif
    pid_t pids[MAX_WINDOWS]  = {0};
    bool  exist[MAX_WINDOWS] = {false};
    bool  alive[MAX_WINDOWS] = {false};
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = g_wins[i];
        exist[i]  = (w != NULL);
        alive[i]  = (w != NULL && w->alive);
        pids[i]   = (w && w->alive) ? w->pty.child : 0;
    }
    /* Under X11_BACKEND the cursor is kept current by x11_backend_render()
     * itself (see render.c / x11_backend.c) — it fires on every content
     * update, whereas this status-bar redraw deliberately doesn't (see
     * the "NOT redrawn on every PTY update" comment further down), so
     * setting the cursor from here would either lag a frame or need its
     * own staleness tracking for no benefit. */
    status_draw(g_rows, g_cols, g_cur, pids, exist, alive);

#ifndef X11_BACKEND
    /*
     * status_draw leaves the real cursor somewhere on the status row.
     * Re-park it at the window's cursor position immediately so it is
     * never visibly stuck in the wrong place between renders.
     * Also update the renderer's cursor accounting so it knows where
     * the real cursor now is and won't emit a redundant CSI H on the
     * next render_screen call.
     */
    Window *_w = g_wins[g_cur];
    if (_w) {
        int r = _w->vt.scr.cur_row, c = _w->vt.scr.cur_col;
        char _mv[32];
        snprintf(_mv, sizeof(_mv), "\033[%d;%dH", r + 1, c + 1);
        (void)write(STDOUT_FILENO, _mv, strlen(_mv));
        g_rs.cursor_row = r;
        g_rs.cursor_col = c;
    }
#endif
}

/* ================================================================== */
/* Window lifecycle                                                     */
/* ================================================================== */

/* Deep-copy a NULL-terminated argv array; returns NULL if src is NULL. */
static char **argv_dup(char *const *src)
{
    if (!src) return NULL;
    int n = 0;
    while (src[n]) n++;
    char **dst = calloc((size_t)(n + 1), sizeof(char *));
    if (!dst) return NULL;
    for (int i = 0; i < n; i++) {
        dst[i] = strdup(src[i]);
        if (!dst[i]) {          /* strdup failed: free what we have */
            for (int j = 0; j < i; j++) free(dst[j]);
            free(dst);
            return NULL;
        }
    }
    return dst;
}

static void argv_free(char **argv)
{
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

/*
 * Create window at slot idx.
 * argv: NULL = run DEFAULT_SHELL/$SHELL; non-NULL = exec that command.
 * The argv is deep-copied so the caller does not need to keep it alive.
 */
static Window *window_create(int idx, char *const *argv)
{
    Window *w = calloc(1, sizeof(Window));
    if (!w) return NULL;

    /* Store a copy first so it survives across respawns */
    w->argv = argv_dup(argv);

    if (pty_open(&w->pty, child_rows(), g_cols, w->argv) < 0) {
        argv_free(w->argv);
        free(w);
        return NULL;
    }

    vt_init(&w->vt, child_rows(), g_cols);
    w->alive    = true;
    w->pristine = (argv == NULL);   /* cleared on first keystroke */
    g_wins[idx] = w;
    return w;
}

static void window_free(int idx)
{
    Window *w = g_wins[idx];
    if (!w) return;
    g_wins[idx] = NULL;
    pty_close(&w->pty);
    vt_free(&w->vt);
    argv_free(w->argv);
    free(w);
}

static void window_inject_msg(Window *w, const char *msg)
{
    vt_feed(&w->vt, (const uint8_t *)msg, strlen(msg));
}

static void __attribute__((unused)) window_mark_dead(int idx)
{
    Window *w = g_wins[idx];
    if (!w) return;
    w->alive      = false;
    pty_close(&w->pty);
    w->pty.master = -1;
    w->pty.child  = 0;
    window_inject_msg(w,
        "\r\n"
        "\033[33m"
        "[Process exited \xe2\x80\x94 press Enter to respawn]"
        "\033[m"
        "\r\n");
}

static bool window_respawn(int idx)
{
    Window *w = g_wins[idx];
    if (!w || w->alive) return false;
    if (pty_open(&w->pty, child_rows(), g_cols, w->argv) < 0) return false;
    vt_free(&w->vt);
    vt_init(&w->vt, child_rows(), g_cols);
    w->alive = true;
    return true;
}

/* ================================================================== */
/* Switching & rendering                                                */
/* ================================================================== */

/* Push current active screen to the observer if one is connected.
 * Silently drops the observer on write error. */
static void observer_push(void)
{
    if (g_observer_fd < 0 || !g_wins[g_cur]) return;
    if (session_observer_push(g_observer_fd, &g_wins[g_cur]->vt.scr,
                               g_wins, g_cur) < 0) {
        close(g_observer_fd);
        g_observer_fd = -1;
    }
}

/* Accept an incoming connection on the listening socket and handle it.
 * 'A' (attach) is only valid when we are in daemon mode; a live instance
 * just closes it.  'O' (observe) sets up the observer fd. */
static void handle_session_conn(void)
{
    int conn = accept(g_session_sock, NULL, NULL);
    if (conn < 0) return;

    uint8_t type = 0;
    if (read(conn, &type, 1) != 1) { close(conn); return; }

    /* Negotiation query: identify ourselves as a live instance */
    if (type == '?') {
        uint8_t live = 'L';
        if (write(conn, &live, 1) != 1) { close(conn); return; }
        if (read(conn, &type, 1) != 1)  { close(conn); return; }
    }

    if (type == SESSION_TYPE_OBSERVE) {
        /* Drop any existing observer first */
        if (g_observer_fd >= 0) close(g_observer_fd);
        g_observer_fd = conn;
        /* Send session size + initial frame */
        if (!g_wins[g_cur] ||
            session_observer_init(conn, &g_wins[g_cur]->vt.scr,
                                  g_rows, g_cols, g_wins, g_cur) < 0) {
            close(g_observer_fd);
            g_observer_fd = -1;
        }
    } else if (type == SESSION_TYPE_TAKEOVER) {
        /*
         * New client wants to become the live instance.
         * Hand over session state, PTY fds, and the session socket.
         * Then schedule downgrade to observer role in the event loop.
         */
        int obs_fd = -1, nr = 0, nc = 0;
        if (session_do_takeover(conn, g_session_sock, g_wins, g_cur,
                                &obs_fd, &nr, &nc) < 0) {
            close(conn);
            return;
        }
        /* New main now owns g_session_sock */
        g_session_sock = -1;
        if (g_observer_fd >= 0) close(g_observer_fd);
        g_observer_fd = -1;
        g_become_observer_rows = nr;
        g_become_observer_cols = nc;
        g_pending_obs_fd       = obs_fd;
    } else {
        close(conn);
    }
}

static void full_redraw(void);  /* forward declaration */

/*
 * Auto-detach sequence shared by SIGHUP and stdin-EOF (controlling
 * terminal closed/hung up by some means other than a clean SSH HUP).
 * Both conditions mean the same thing: the terminal is gone, so behave
 * exactly like an explicit PREFIX+d.  Never returns.
 */
static void auto_detach(void)
{
    if (g_observer_fd >= 0) {
        /*
         * Best-effort notification: use a non-blocking send so a stalled
         * (not cleanly closed) observer connection can never hang the
         * detach sequence.  If it doesn't go through, the observer will
         * simply see the connection drop and exit on its own — we still
         * complete daemonization either way.
         */
        ObsHeader _sig;
        memset(&_sig, 0, sizeof(_sig));
        _sig.rows = -1;
        send(g_observer_fd, &_sig, sizeof(_sig), MSG_DONTWAIT);
        close(g_observer_fd);
        g_observer_fd = -1;
    }
#ifndef X11_BACKEND
    (void)write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    input_restore();
#endif
    render_free(&g_rs);
#ifdef X11_BACKEND
    /* The daemon is forked from here and would inherit our X connection,
     * keeping a dead window on screen: close it first. */
    x11_backend_shutdown();
#endif
    session_detach(g_session_sock, g_wins, g_cur);
}

#if SCROLLBACK_ENABLED
/* One line of the scrollback view.  On a real terminal (the plain
 * build) it is written to stdout, dimmed if asked.  Under X11_BACKEND
 * there is no terminal to write to (see the note by the main select()
 * loop), so it is drawn into the window's cells instead; the backend
 * wraps and scrolls just as a terminal would, so the two builds show the
 * same thing. */
static void sb_line(const char *p, int n, bool dim)
{
#ifdef X11_BACKEND
    x11_backend_text_line(p, n, (Style){ dim ? 8 : 15, 0 });
#else
    if (dim)  (void)write(STDOUT_FILENO, "\033[2m", 4);
    if (n > 0) (void)write(STDOUT_FILENO, p, (size_t)n);
    if (dim)  (void)write(STDOUT_FILENO, "\033[m", 3);
    (void)write(STDOUT_FILENO, "\r\n", 2);
#endif
}

/*
 * Render the scrollback history for the current window.
 * Displays up to child_rows() lines of captured history ending at
 * g_scrollback_offset lines from the most-recent captured line.
 * Lines are plain UTF-8.
 */
static void render_scrollback_view(void)
{
    Window *w = g_wins[g_cur];
    if (!w || !w->vt.scr.scrollback) { full_redraw(); return; }
    Screen *s = &w->vt.scr;

    int view_rows = child_rows();
    const char *buf = s->scrollback;
    int len = s->scrollback_len;

    /* Count lines: scrollback buffer + live screen rows.
     * Live screen rows are encoded on-the-fly at render time — no copy. */
    int sb_lines = 0;
    for (int i = 0; i < len; i++)
        if (buf[i] == '\n') sb_lines++;
    int live_lines = s->rows;
    int total = sb_lines + live_lines;

    /* Clamp offset: 0 = bottom (live screen visible), higher = further back */
    int max_off = total > view_rows ? total - view_rows : 0;
    if (g_scrollback_offset > max_off) g_scrollback_offset = max_off;
    if (g_scrollback_offset < 0)       g_scrollback_offset = 0;

    /* Lines to skip from the very beginning of the combined virtual buffer */
    int skip = total - view_rows - g_scrollback_offset;
    if (skip < 0) skip = 0;

    /* Clear screen and home cursor */
#ifdef X11_BACKEND
    x11_backend_text_begin();
#else
    (void)write(STDOUT_FILENO, "\033[2J\033[H", 8);
#endif

    int row = 0;

    /* Top-of-history marker */
    if (skip == 0) {
        sb_line("-- top of scrollback --", 23, true);
        row++;
    }

    /* Phase 1: lines from the scrollback buffer */
    int sb_skip = skip < sb_lines ? skip : sb_lines;
    int pos = 0;
    for (int i = 0; i < sb_skip && pos < len; pos++)
        if (buf[pos] == '\n') i++;

    int sb_row = sb_skip;   /* which sb line we're on */
    while (row < view_rows && sb_row < sb_lines && pos <= len) {
        int end = pos;
        while (end < len && buf[end] != '\n') end++;
        sb_line(buf + pos, end - pos, false);
        row++; sb_row++;
        pos = end + 1;
    }

    /* Phase 2: live screen rows (encoded on the fly, no allocation) */
    int live_skip = skip > sb_lines ? skip - sb_lines : 0;
    for (int r = live_skip; r < s->rows && row < view_rows; r++, row++) {
        /* Find last non-space cell for trimming */
        int last = -1;
        for (int c = 0; c < s->cols; c++) {
            Cell *cell = &s->cells[r][c];
            if (cell->flags & CELL_WIDE_CONT) continue;
            uint32_t cp = cell->ch ? cell->ch : ' ';
            if (cp != ' ') last = c;
        }
        /* Encode cells up to last non-space */
        char line[(size_t)s->cols * 4];
        int  ll = 0;
        for (int c = 0; c <= last && c < s->cols; c++) {
            Cell *cell = &s->cells[r][c];
            if (cell->flags & CELL_WIDE_CONT) continue;
            uint32_t cp = cell->ch ? cell->ch : ' ';
            if      (cp < 0x80)    { line[ll++] = (char)cp; }
            else if (cp < 0x800)   { line[ll++] = (char)(0xC0|(cp>>6));
                                     line[ll++] = (char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { line[ll++] = (char)(0xE0|(cp>>12));
                                     line[ll++] = (char)(0x80|((cp>>6)&0x3F));
                                     line[ll++] = (char)(0x80|(cp&0x3F)); }
            else                   { line[ll++] = (char)(0xF0|(cp>>18));
                                     line[ll++] = (char)(0x80|((cp>>12)&0x3F));
                                     line[ll++] = (char)(0x80|((cp>>6)&0x3F));
                                     line[ll++] = (char)(0x80|(cp&0x3F)); }
        }
        sb_line(line, ll, false);
    }

    /* Scrollback status indicator in the status row, in the status
     * bar's own colours (config.h) */
    char ind_text[64];
    int ilen = snprintf(ind_text, sizeof(ind_text),
                        "  %d/%d lines  (q/Esc to exit) ",
                        g_scrollback_offset, sb_lines);
#ifdef X11_BACKEND
    x11_backend_text_fill(g_rows - 1, STATUS_BAR_COLOR);
    x11_backend_text_at(g_rows - 1, 0, " SCROLLBACK ", 12,
                        WINDOW_STATUS_CURRENT_COLOR);
    x11_backend_text_at(g_rows - 1, 12, ind_text, ilen, STATUS_BAR_COLOR);
    x11_backend_text_end();
#else
    static const char sb_prefix[] =
        STATUS_BAR_STYLE "\033[2K"
        WINDOW_STATUS_CURRENT_STYLE " SCROLLBACK "
        STATUS_BAR_STYLE;
    char ind_pos[24];
    snprintf(ind_pos, sizeof(ind_pos), "\033[%d;1H", g_rows);
    (void)write(STDOUT_FILENO, ind_pos, strlen(ind_pos));
    (void)write(STDOUT_FILENO, sb_prefix, sizeof(sb_prefix) - 1);
    (void)write(STDOUT_FILENO, ind_text, (size_t)ilen);
    (void)write(STDOUT_FILENO, _RESET, sizeof(_RESET) - 1);

    /* Park cursor out of the way */
    char mv[16];
    snprintf(mv, sizeof(mv), "\033[%d;1H", view_rows);
    (void)write(STDOUT_FILENO, mv, strlen(mv));
#endif
}

/*
 * Handle one raw byte of stdin while in scrollback mode.
 * Returns true when scrollback mode should exit.
 *
 * Manages a tiny state machine for multi-byte escape sequences:
 *   ESC alone → exit
 *   ESC [ A   → up 1
 *   ESC [ B   → down 1
 *   ESC [ 5 ~ → PgUp
 *   ESC [ 6 ~ → PgDn
 */
static int  _sb_esc  = 0;   /* 0=ground 1=saw ESC 2=saw ESC[ 3=saw ESC[5 4=saw ESC[6 */
static bool scrollback_input_byte(uint8_t b)
{
    int view_rows = child_rows();

    switch (_sb_esc) {
    case 0:
        if (b == '\033') { _sb_esc = 1; return false; }
        if (b == '\003' || b == 'q') { _sb_esc = 0; return true; }  /* Ctrl-C, q */
        return false;
    case 1:
        if (b == '[') { _sb_esc = 2; return false; }
        _sb_esc = 0;
        return true;   /* bare ESC or unknown → exit */
    case 2:
        _sb_esc = 0;
        if (b == 'A') { g_scrollback_offset++;          render_scrollback_view(); }
        else if (b == 'B') { g_scrollback_offset--;     render_scrollback_view(); }
        else if (b == '5') { _sb_esc = 3; return false; }
        else if (b == '6') { _sb_esc = 4; return false; }
        return false;
    case 3:  /* ESC[5 → wait for ~ */
        _sb_esc = 0;
        if (b == '~') { g_scrollback_offset += view_rows - 1; render_scrollback_view(); }
        return false;
    case 4:  /* ESC[6 → wait for ~ */
        _sb_esc = 0;
        if (b == '~') { g_scrollback_offset -= view_rows - 1; render_scrollback_view(); }
        return false;
    }
    return false;
}

/* Give a key byte to the scrollback viewer if it is open; returns true
 * if it consumed the byte.  `last` is true for the final byte of a batch
 * of input: under X11_BACKEND keys arrive whole (an arrow key is one
 * batch of three bytes), so an ESC that ends a batch is the Escape key
 * itself and closes the viewer at once, rather than waiting for a next
 * key to show it wasn't the start of a sequence.  (Not done for a real
 * terminal, where a sequence can be split across reads.) */
static bool scrollback_feed(uint8_t b, bool last)
{
    if (!g_scrollback_mode) return false;
    bool exit_sb = scrollback_input_byte(b);
#ifdef X11_BACKEND
    if (b == '\033' && last && _sb_esc == 1) exit_sb = true;
#else
    (void)last;
#endif
    if (exit_sb) {
        g_scrollback_mode   = false;
        g_scrollback_offset = 0;
        _sb_esc             = 0;
        full_redraw();
    }
    return true;
}
#endif /* SCROLLBACK_ENABLED */

static void full_redraw(void)
{
#ifdef X11_BACKEND
    (void)x11_backend_selection_drop();
#endif
#if SCROLLBACK_ENABLED
    if (g_scrollback_mode) { render_scrollback_view(); return; }
#endif
    render_full_redraw(&g_rs, &g_wins[g_cur]->vt.scr);
    redraw_status();
    observer_push();
}

static void switch_to(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (!g_wins[idx])
        if (!window_create(idx, NULL)) return;

    /* Auto-close the outgoing window if it was a default shell that
     * never received any input — treat it as an accidental open. */
    int prev = g_cur;
    if (prev != idx && g_wins[prev] && g_wins[prev]->pristine)
        window_free(prev);

    g_cur = idx;

    /* Exit scrollback mode when switching windows */
    g_scrollback_mode = false;
    g_scrollback_offset = 0;
    full_redraw();
}

/* ================================================================== */
/* PTY drain                                                            */
/* ================================================================== */

static bool drain_pty(Window *w)
{
    uint8_t buf[4096];
    ssize_t n;
    bool    got_data = false;

    while ((n = read(w->pty.master, buf, sizeof(buf))) > 0) {
        vt_feed(&w->vt, buf, (size_t)n);
        got_data = true;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        w->alive = false;

    return got_data;
}

/* ================================================================== */
/* Clean-up                                                             */
/* ================================================================== */

static void cleanup(void)
{
#ifndef X11_BACKEND
    /* Ensure cursor is visible in the parent terminal regardless of what
     * the active window's application had set. Under X11_BACKEND we never
     * touch the parent tty in the first place, so there's nothing here to
     * undo. */
    (void)write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    input_restore();
#endif
    render_free(&g_rs);
    for (int i = 0; i < MAX_WINDOWS; i++)
        window_free(i);
    if (g_observer_fd >= 0) {
        close(g_observer_fd);
        g_observer_fd = -1;
    }
    if (g_session_sock >= 0) {
        close(g_session_sock);
        unlink(MUX_SOCKET_PATH);
        g_session_sock = -1;
    }
}

static void die(const char *msg)
{
    cleanup();
    perror(msg);
    exit(1);
}

/* ================================================================== */
/* SIGWINCH                                                             */
/* ================================================================== */

static void handle_resize(void)
{
    get_term_size(&g_rows, &g_cols);
    int crows = child_rows();

    /* Resize the renderer shadow first so it matches the new geometry. */
    render_free(&g_rs);
    render_init(&g_rs, crows, g_cols);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = g_wins[i];
        if (!w) continue;
        /* Resize the VT screen buffer for every window, alive or dead.
         * A dead window may be respawned later and needs consistent
         * dimensions; its pty.master is -1 so pty_resize is skipped. */
        vt_resize(&w->vt, crows, g_cols);
        if (w->alive && w->pty.master >= 0)
            pty_resize(&w->pty, crows, g_cols);
    }

    full_redraw();
}

/* ================================================================== */
/* Input dispatch                                                       */
/* ================================================================== */

/* Set when pty_write_all read the window's output to get on: the screen
 * is behind and the main loop must repaint (and answer what it asked). */
static bool g_pty_drained;

/* Write all of buf to the window's terminal.  The master is non-blocking
 * and the tty's input queue small (about 4 KB), so a long text -- a paste --
 * does not go in one write: the program has to read some first.  While
 * waiting for it, its output is read, since it may be blocked writing that
 * to us.  If nothing moves for a second the program is not reading at all
 * and the rest is dropped. */
static void pty_write_all(Window *w, const uint8_t *b, size_t n)
{
    int stalled = 0;
    while (n) {
        ssize_t r = write(w->pty.master, b, n);
        if (r > 0) { b += r; n -= (size_t)r; stalled = 0; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno != EAGAIN) return;           /* the program is gone */
        fd_set rf, wf;
        FD_ZERO(&rf); FD_ZERO(&wf);
        FD_SET(w->pty.master, &rf); FD_SET(w->pty.master, &wf);
        struct timeval tv = { 0, 100000 };
        int ready = select(w->pty.master + 1, &rf, &wf, NULL, &tv);
        if (ready > 0 && FD_ISSET(w->pty.master, &rf) && drain_pty(w))
            g_pty_drained = true;
        if (ready <= 0 && ++stalled >= 10) return;
        if (ready > 0) stalled = 0;
    }
}

static void write_to_active(const uint8_t *buf, size_t n)
{
    Window *w = g_wins[g_cur];
    if (!w) return;

    if (!w->alive) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                if (window_respawn(g_cur)) {
                    full_redraw();
                }
            }
        }
        return;
    }

    w->pristine = false;   /* user has typed something */

    /* DECCKM: translate ESC [ A/B/C/D → ESC O A/B/C/D */
    static uint8_t seq[3];
    static int     seq_len;

    /* Nothing to translate: all at once (a paste can be long). */
    if (!w->vt.scr.app_cursor && seq_len == 0) {
        pty_write_all(w, buf, n);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (!w->vt.scr.app_cursor) {
            pty_write_all(w, &b, 1);
            seq_len = 0;
            continue;
        }
        seq[seq_len++] = b;
        if (seq_len == 1 && seq[0] != 0x1B) {
            pty_write_all(w, seq, 1); seq_len = 0;
        } else if (seq_len == 2) {
            if (seq[1] != '[' && seq[1] != 'O') {
                pty_write_all(w, seq, 2); seq_len = 0;
            }
        } else if (seq_len == 3) {
            if (seq[1]=='[' && (seq[2]=='A'||seq[2]=='B'||
                                 seq[2]=='C'||seq[2]=='D')) {
                uint8_t out[3] = { 0x1B, 'O', seq[2] };
                pty_write_all(w, out, 3);
            } else {
                pty_write_all(w, seq, 3);
            }
            seq_len = 0;
        }
    }
}

#ifdef X11_BACKEND
/* What the displayed program asked for with ?1000/?1002/?1003 (0 when it
 * has gone). */
static int shown_mouse_mode(void)
{
    Window *w = g_wins[g_cur];
    return w && w->alive ? w->vt.scr.mouse_mode : 0;
}

/* How much pointer traffic to ask the X server for: what that program
 * wants, but at least buttons and drags, since the mouse is also ours for
 * copy and paste (see x11_backend.c) -- except while the scrollback
 * viewer is up, which has nothing to select. */
static int mouse_level_wanted(void)
{
    if (!g_wins[g_cur]) return 0;
#if SCROLLBACK_ENABLED
    if (g_scrollback_mode) return 0;
#endif
    int m = shown_mouse_mode();
    return m < 2 ? 2 : m;
}

/* Where what the pointer produces for the program (reports, pasted text)
 * goes: its terminal, if it is still there. */
static void pointer_sink(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    Window *w = g_wins[g_cur];
    if (w && w->alive) { w->pristine = false; pty_write_all(w, b, n); }
}
#endif

static void handle_event(InputEvent ev)
{
    switch (ev.cmd) {
    case CMD_PASS_BYTE: write_to_active(&ev.byte, 1); break;
    case CMD_PASS_TWO: {
        uint8_t two[2] = { ev.byte, ev.byte2 };
        write_to_active(two, 2);
        break;
    }
    case CMD_SELECT_WINDOW: switch_to(ev.arg); break;
    case CMD_NEXT_WINDOW:   switch_to((g_cur + 1) % MAX_WINDOWS); break;
    case CMD_PREV_WINDOW:   switch_to((g_cur + MAX_WINDOWS - 1) % MAX_WINDOWS); break;
    case CMD_NEW_WINDOW:
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (!g_wins[i]) { switch_to(i); break; }
        break;
    case CMD_SCROLLBACK_ENTER:
    case CMD_SCROLLBACK_ENTER_PGUP:
#if SCROLLBACK_ENABLED
        if (g_wins[g_cur] && g_wins[g_cur]->vt.scr.scrollback &&
                g_wins[g_cur]->vt.scr.scrollback_len > 0) {
            g_scrollback_mode   = true;
            _sb_esc             = 0;
            /* PgUp variant: pre-scroll one page so history is immediately
             * visible without an extra keypress. */
            g_scrollback_offset = (ev.cmd == CMD_SCROLLBACK_ENTER_PGUP)
                                  ? (child_rows() - 1) : 0;
            render_scrollback_view();
        }
#endif
        break;
    case CMD_CLOSE_WINDOW: window_free(g_cur); {
        int next = -1;
        for (int j = 0; j < MAX_WINDOWS; j++)
            if (g_wins[j] && g_wins[j]->alive) { next = j; break; }
        if (next >= 0) switch_to(next);
        else g_quit = 1;
        break;
    }
    case CMD_REDRAW:  full_redraw(); break;
    case CMD_QUIT:    g_quit = 1;    break;
    case CMD_DETACH:
        auto_detach();   /* never returns */
        break;
    case CMD_NONE:    break;
    }
}

/* ================================================================== */
/* Main                                                                 */
/* ================================================================== */

/* ================================================================== */
/* Command-line argument parsing                                        */
/* ================================================================== */

/*
 * Commands are separated by ";" tokens (the shell turns \; into ;).
 * Each command is one of the aliases for "new window":
 *   new | neww | new-window | new-session
 * followed by an optional argv that is exec'd in that window.
 *
 * Example:
 *   mux new top -d1 \; neww tail -f /var/log/syslog \; neww
 */
static bool is_new_cmd(const char *s)
{
    return strcmp(s, "new")         == 0
        || strcmp(s, "neww")        == 0
        || strcmp(s, "new-window")  == 0
        || strcmp(s, "new-session") == 0;
}

/*
 * Parse argv[1..argc-1] into a list of window specs.
 * Each spec is a NULL-terminated argv array (or NULL for default shell).
 * Returns the number of windows to open (>=1); fills out_specs[0..n-1].
 * out_specs must have room for MAX_WINDOWS entries.
 *
 * If there are no arguments at all, returns 1 with out_specs[0] = NULL
 * (one default-shell window, the normal startup behaviour).
 */
static int parse_args(int argc, char *argv[],
                      char **out_specs[MAX_WINDOWS])
{
    if (argc < 2) {
        out_specs[0] = NULL;
        return 1;
    }

    int nwin = 0;
    int i = 1;
    while (i < argc && nwin < MAX_WINDOWS) {
        if (!is_new_cmd(argv[i])) {
            fprintf(stderr, "mux: unknown command '%s'\n", argv[i]);
            fprintf(stderr, "usage: mux [new|neww [cmd [args]] [; new|neww ...]]\n");
            exit(1);
        }
        i++;   /* skip the command keyword */

        /* Collect arguments until next ";" or end */
        int arg_start = i;
        while (i < argc && strcmp(argv[i], ";") != 0)
            i++;

        int arg_count = i - arg_start;
        if (arg_count == 0) {
            out_specs[nwin++] = NULL;   /* default shell */
        } else {
            /* NULL-terminated array pointing into argv (no string copies
             * needed: argv is valid for the lifetime of the process) */
            char **spec = calloc((size_t)(arg_count + 1), sizeof(char *));
            if (!spec) { perror("mux"); exit(1); }
            for (int j = 0; j < arg_count; j++)
                spec[j] = argv[arg_start + j];
            spec[arg_count] = NULL;
            out_specs[nwin++] = spec;
        }

        if (i < argc && strcmp(argv[i], ";") == 0)
            i++;   /* skip ";" separator */
    }

    return nwin > 0 ? nwin : 1;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    /* Install our signal dispositions before anything below can fork a
     * child (window_create() does, for the very first window). Signal
     * dispositions are inherited across fork(), and exec() only resets
     * *caught* signals back to default -- SIG_IGN survives exec(). If mux
     * is ever started as a backgrounded/non-interactive job (a service
     * manager, a script, `cmd &` under a non-interactive shell, or -- as
     * hit during this project's own X11-backend testing -- a test harness
     * driving mux under Xvfb), SIGINT and SIGQUIT are commonly already
     * SIG_IGN at that point. Left unhandled until after the first
     * window's shell is forked, that ignore-disposition would leak all
     * the way down through the shell into every program it runs,
     * silently breaking Ctrl-C for the entire first window for the life
     * of the session. Setting our own dispositions first closes that
     * window: SIGINT/SIGTERM/SIGHUP get real handlers (caught, so exec()
     * resets them to default for children regardless of what was
     * inherited), and SIGPIPE is IGNORE, but consistently for every
     * window rather than only windows created after this point. */
    signal(SIGWINCH, on_sigwinch);
    signal(SIGCHLD,  on_sigchld);
    signal(SIGTERM,  on_sigterm);
    signal(SIGINT,   on_sigterm);
    signal(SIGHUP,   on_sighup);
    signal(SIGPIPE,  SIG_IGN);

#ifdef X11_BACKEND
    if (x11_backend_init() < 0) return 1;
#endif

    bool attach_mode = (argc >= 2 && strcmp(argv[1], "attach") == 0);

    if (attach_mode) {
        /*
         * Attach to a running daemon.  session_attach() populates g_wins[]
         * and g_cur from the daemon's serialized state + PTY master fds.
         * We then resize everything to the current terminal dimensions.
         */
        int new_sess_fd = -1, obs_fd = -1;
        int attach_rc = session_attach(g_wins, &g_cur, &new_sess_fd, &obs_fd);
        if (attach_rc == -2) return 0;   /* observed and done */
        if (attach_rc < 0) {
            fputs("mux: attach failed\n", stderr);
            return 1;
        }
        /* Takeover: adopt the session socket and observer channel */
        if (obs_fd >= 0) g_observer_fd = obs_fd;
        if (new_sess_fd >= 0) {
            /* Role-reversal: old-live handed us its session socket. */
            g_session_sock = new_sess_fd;
        } else {
            /* Daemon takeover: the daemon exits as soon as it has our ACK,
             * but may still be listening for an instant, so session_listen()'s
             * liveness probe would sometimes find it and refuse.  Rebind
             * unconditionally, as the observer promotion below does. */
            g_session_sock = session_rebind();
            if (g_session_sock < 0) {
                fputs("mux: failed to create session socket\n", stderr);
                return 1;
            }
        }
#ifdef X11_BACKEND
        g_rows = x11_backend_rows(); g_cols = x11_backend_columns();
#else
        get_term_size(&g_rows, &g_cols);
#endif
        render_init(&g_rs, child_rows(), g_cols);
#ifndef X11_BACKEND
        (void)write(STDOUT_FILENO, "\033[?1049h", 8);
#endif

        int crows = child_rows();
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!g_wins[i]) continue;
            vt_resize(&g_wins[i]->vt, crows, g_cols);
            if (g_wins[i]->alive && g_wins[i]->pty.master >= 0)
                pty_resize(&g_wins[i]->pty, crows, g_cols);
        }
    } else {
        /*
         * Normal startup.  session_listen() binds the socket immediately;
         * if another session is already listening it will fail with EADDRINUSE,
         * which is how we prevent two independent sessions from fighting.
         */
        g_session_sock = session_listen();
        if (g_session_sock < 0) {
            /* session_listen printed the error; check for the common case */
            if (errno == EADDRINUSE)
                fputs("mux: session already exists (use 'mux attach')\n", stderr);
            return 1;
        }

#ifdef X11_BACKEND
        g_rows = x11_backend_rows(); g_cols = x11_backend_columns();
#else
        get_term_size(&g_rows, &g_cols);
#endif

        /* Parse command-line window specs before entering raw mode so any
         * usage errors print cleanly to the normal terminal. */
        char **win_specs[MAX_WINDOWS];
        int    win_count = parse_args(argc, argv, win_specs);

        render_init(&g_rs, child_rows(), g_cols);
#ifndef X11_BACKEND
        (void)write(STDOUT_FILENO, "\033[?1049h", 8);
#endif

        int first_slot = -1;
        for (int w = 0; w < win_count; w++) {
            int slot = -1;
            for (int s = 0; s < MAX_WINDOWS; s++)
                if (!g_wins[s]) { slot = s; break; }
            if (slot < 0) break;

            if (!window_create(slot, (char *const *)win_specs[w])) {
#ifndef X11_BACKEND
                (void)write(STDOUT_FILENO, "\033[?1049l", 8);
#endif
                fprintf(stderr, "mux: failed to create window %d\n", w + 1);
                return 1;
            }
            if (first_slot < 0) first_slot = slot;
            free(win_specs[w]);
        }
        g_cur = first_slot >= 0 ? first_slot : 0;
    }

#ifndef X11_BACKEND
    input_raw_mode();
#endif

    /* Full redraw on attach (shadow is empty); status-only for fresh start */
    if (attach_mode)
        full_redraw();
    else
        redraw_status();

    /*
     * Countdown timers.  Rather than calling clock_gettime() on every
     * loop iteration, we let Linux's select() tell us how much time is
     * left in the timeval after it returns.  We subtract that from our
     * countdown timers.  No clock syscalls needed in the hot path.
     *
     * On non-Linux platforms we fall back to now_ms().
     */
    long clock_remaining_ms = ms_to_next_minute();
    long esc_remaining_ms   = -1;   /* -1 = no ESC pending */

    while (!g_quit) {

        if (g_winch) { g_winch = 0; handle_resize(); }

#ifdef X11_BACKEND
        /* Our "terminal" went away: the window manager asked us to close
         * (the close button, Alt-F4...) or the X connection was lost (the
         * server exited, an `ssh -X` link dropped).  Same as the tty
         * hanging up under the plain build: detach, so the session lives
         * on -- handed to the other client if there is one, else as a
         * daemon for a later `attach`.  (Quitting is Ctrl+B q.) */
        if (x11_backend_close_requested() || x11_backend_lost())
            auto_detach();   /* never returns */

        /* Pointer events are only requested from the X server while the
         * displayed program wants them (it can change on any PTY output
         * or window switch; the call is free when nothing changed). */
        x11_backend_mouse_select(mouse_level_wanted());
#endif

        /* Downgrade to observer after a takeover handshake */
        if (g_become_observer_rows > 0) {
            int obs_conn = g_pending_obs_fd;
            g_pending_obs_fd = -1;
            int br = g_become_observer_rows;
            int bc = g_become_observer_cols;
            g_become_observer_rows = 0;
            g_become_observer_cols = 0;
            /* Free all windows — new main holds the PTY masters now */
            input_restore();
            render_free(&g_rs);
            for (int _i = 0; _i < MAX_WINDOWS; _i++) {
                if (!g_wins[_i]) continue;
                /* Close PTY without freeing VT (new main owns the master) */
                g_wins[_i]->pty.master = -1;
                window_free(_i);
            }
            /* Enter observe mode over the takeover connection.
             * Returns 1 if the new-live itself detached (rows=-1 sentinel)
             * while we were observing — in that case take over again. */
            int obs_ret = session_become_observer(obs_conn, br, bc);
            if (obs_ret == 1) {
                /* Re-init enough state to run session_attach */
#ifdef X11_BACKEND
                g_rows = x11_backend_rows(); g_cols = x11_backend_columns();
#else
                get_term_size(&g_rows, &g_cols);
#endif
                int ns = -1, of2 = -1;
                if (session_attach(g_wins, &g_cur, &ns, &of2) < 0) {
                    g_quit = 1; break;
                }
                if (ns >= 0) {
                    /* Role-reversal takeover: use the inherited socket fd */
                    g_session_sock = ns;
                } else {
                    /* Daemon takeover: the daemon is about to exit and its
                     * socket is still briefly alive.  Skip the liveness
                     * probe (session_listen) — use session_rebind which
                     * unlinks and binds unconditionally. */
                    if (g_session_sock >= 0) {
                        close(g_session_sock);
                        g_session_sock = -1;
                    }
                    g_session_sock = session_rebind();
                    if (g_session_sock < 0) { g_quit = 1; break; }
                }
                if (of2 >= 0) g_observer_fd = of2;
                render_init(&g_rs, child_rows(), g_cols);
#ifndef X11_BACKEND
                (void)write(STDOUT_FILENO, "\033[?1049h", 8);
                input_raw_mode();
#endif
                signal(SIGWINCH, on_sigwinch);
                signal(SIGCHLD,  on_sigchld);
                signal(SIGTERM,  on_sigterm);
                signal(SIGINT,   on_sigterm);
                signal(SIGHUP,   on_sighup);
                signal(SIGPIPE,  SIG_IGN);
                int crows = child_rows();
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (!g_wins[i]) continue;
                    vt_resize(&g_wins[i]->vt, crows, g_cols);
                    if (g_wins[i]->alive && g_wins[i]->pty.master >= 0)
                        pty_resize(&g_wins[i]->pty, crows, g_cols);
                }
                full_redraw();
                continue;   /* re-enter event loop */
            }
            g_quit = 1;
            break;
        }

        if (g_sighup) {
            g_sighup = 0;
            auto_detach();   /* never returns */
        }

        if (g_child) {
            g_child = 0;
            int status; pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (!g_wins[i] || g_wins[i]->pty.child != pid) continue;
                    Window *w = g_wins[i];
                    drain_pty(w);
#if REMAIN_ON_EXIT
                    window_mark_dead(i);
                    if (i == g_cur) render_screen(&g_rs, &w->vt.scr);
                    redraw_status();
#else
                    /* Close the window immediately */
                    window_free(i);
                    /* If this was the active window, switch to another */
                    if (i == g_cur) {
                        int next = -1;
                        for (int j = 0; j < MAX_WINDOWS; j++)
                            if (g_wins[j] && g_wins[j]->alive) { next = j; break; }
                        if (next >= 0)
                            switch_to(next);
                        else
                            g_quit = 1;   /* no live windows left */
                    } else {
                        redraw_status();  /* update tab list */
                    }
#endif
                    break;
                }
            }
            /* The last window may just have gone: leave now instead of
             * blocking in select() until some unrelated event (under
             * X11_BACKEND, any key press) wakes us up. */
            if (g_quit) continue;
        }

        /* Build fd set */
        fd_set rfds;
        FD_ZERO(&rfds);
#ifdef X11_BACKEND
        /* X11_BACKEND is a self-contained GUI program: all user
         * interaction goes through its own window, never through
         * whatever real terminal happened to launch it (which may not
         * even have a real user attached — service manager, script,
         * CI). We deliberately never read the parent tty's stdin at
         * all. The one exception is Ctrl-C, and that needs no code
         * here: since we never touch the parent tty's termios (no raw
         * mode), it stays in its normal cooked/ISIG mode, so the
         * kernel itself turns a Ctrl-C typed there into a real SIGINT
         * delivered straight to this process, handled the same as any
         * other SIGINT (see on_sigterm). */
        int maxfd = -1;
#else
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;
#endif
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window *w = g_wins[i];
            if (w && w->alive && w->pty.master >= 0) {
                FD_SET(w->pty.master, &rfds);
                if (w->pty.master > maxfd) maxfd = w->pty.master;
            }
        }
        if (g_session_sock >= 0) {
            FD_SET(g_session_sock, &rfds);
            if (g_session_sock > maxfd) maxfd = g_session_sock;
        }
        if (g_observer_fd >= 0) {
            FD_SET(g_observer_fd, &rfds);
            if (g_observer_fd > maxfd) maxfd = g_observer_fd;
        }
#ifdef X11_BACKEND
        /* Watch the X11 connection itself so KeyPress/Expose activity
         * wakes select() promptly instead of waiting for unrelated PTY
         * output, real-stdin input, or the (up to ~60s) clock timer. */
        int x11fd = x11_backend_fd();
        if (x11fd >= 0) {
            FD_SET(x11fd, &rfds);
            if (x11fd > maxfd) maxfd = x11fd;
        }
#endif

        /*
         * ESC disambiguation: if a bare ESC is sitting in the input buffer,
         * start its timer NOW — before we compute the select() timeout.
         * Previously this was done after select(), which meant the first
         * loop iteration after reading ESC used clock_remaining (up to ~60s)
         * as the timeout instead of ESC_TIMEOUT_MS, so bare ESC was delayed
         * by up to a full minute rather than 20ms.
         */
        if (input_esc_pending() && esc_remaining_ms < 0)
            esc_remaining_ms = ESC_TIMEOUT_MS;

        /* Flush ESC immediately if already expired (e.g. set to 0 above
         * because ESC_TIMEOUT_MS == 0, or carried over from last iter) */
        if (esc_remaining_ms == 0) {
            handle_event(input_flush_esc());
            esc_remaining_ms = -1;
            continue;
        }

        /* Select timeout = min(clock_remaining, esc_remaining) */
        long timeout_ms = clock_remaining_ms;
        if (esc_remaining_ms >= 0 && esc_remaining_ms < timeout_ms)
            timeout_ms = esc_remaining_ms;
        if (timeout_ms < 0) timeout_ms = 0;

        struct timeval tv = {
            .tv_sec  = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) { if (errno == EINTR) continue; die("select"); }

        /*
         * Subtract elapsed time from our countdown timers.
         *
         * On Linux, select() updates tv with the time remaining, so
         * elapsed = timeout_ms - remaining.  No clock syscall needed.
         * On other platforms, use now_ms() as a fallback.
         */
#ifdef __linux__
        long remaining_ms = tv.tv_sec * 1000L + tv.tv_usec / 1000L;
        long elapsed_ms   = timeout_ms - remaining_ms;
        if (elapsed_ms < 0) elapsed_ms = 0;
#else
        static long _last_ms;
        long _now = now_ms();
        long elapsed_ms = (_last_ms > 0) ? (_now - _last_ms) : 0;
        _last_ms = _now;
#endif

        clock_remaining_ms -= elapsed_ms;
        if (esc_remaining_ms > 0) esc_remaining_ms -= elapsed_ms;

        /* Flush ESC if its timer just expired */
        if (input_esc_pending() && esc_remaining_ms <= 0) {
            handle_event(input_flush_esc());
            esc_remaining_ms = -1;
        }

#ifdef X11_BACKEND
        /* X11 activity: Expose (needs a repaint — the server may have
         * discarded the window's contents while another window covered
         * it), a resize (new grid size: tell every window and pty) and/or KeyPress (queued as translated bytes by xterm_wait,
         * fed through the same FSM as real-stdin bytes below). */
        if (x11fd >= 0 && FD_ISSET(x11fd, &rfds)) {
            if (x11_backend_wait(0)) {
                if (x11_backend_rows() != g_rows ||
                    x11_backend_columns() != g_cols)
                    handle_resize();     /* the window was resized */
                else if (g_wins[g_cur]) {
                    if (!g_scrollback_mode)
                        render_full_redraw(&g_rs, &g_wins[g_cur]->vt.scr);
                    redraw_status();   /* (repaints the viewer, if open) */
                }
            }
            uint8_t kbuf[64];
            int kn = x11_backend_read_input(kbuf, sizeof kbuf);
            /* Typing ends a text selection's highlight. */
            if (kn > 0 && g_wins[g_cur] && !g_scrollback_mode &&
                x11_backend_selection_drop())
                render_screen(&g_rs, &g_wins[g_cur]->vt.scr);
            for (int i = 0; i < kn; i++) {
#if SCROLLBACK_ENABLED
                /* a full buffer might end mid-sequence: not a lone ESC */
                if (scrollback_feed(kbuf[i], i == kn - 1 && kn < (int)sizeof kbuf))
                    continue;
#endif
                InputEvent ev = input_feed(kbuf[i]);
                if (!input_esc_pending()) esc_remaining_ms = -1;
                handle_event(ev);
            }

            /* Pointer events: for the displayed program, as the report it
             * asked for, or else text selection, copy and paste. */
            {
                Window *mw = g_wins[g_cur];
                if (x11_backend_paste_key() && mw && !g_scrollback_mode)   /* Shift+Insert, Ctrl+V */
                    x11_backend_paste(mw->vt.scr.bracketed_paste, pointer_sink, NULL);
                if (mw && !g_scrollback_mode) {
                    Screen *ms = &mw->vt.scr;
                    x11_backend_pointer(ms, shown_mouse_mode(), ms->mouse_enc,
                                        ms->bracketed_paste, child_rows(), g_cols,
                                        pointer_sink, NULL);
                }
            }
        }
#endif

        /* Incoming session connection (observe or stray attach) */
        if (g_session_sock >= 0 && FD_ISSET(g_session_sock, &rfds))
            handle_session_conn();

        /* Observer commands: parse K/W typed protocol */
        if (g_observer_fd >= 0 && FD_ISSET(g_observer_fd, &rfds)) {
            uint8_t type;
            ssize_t r = read(g_observer_fd, &type, 1);
            if (r <= 0) {
                close(g_observer_fd);
                g_observer_fd = -1;
            } else if (type == 'K') {
                /* Raw key bytes for active PTY */
                uint8_t klen;
                if (read(g_observer_fd, &klen, 1) == 1 && klen > 0) {
                    char kbuf[256];
                    /* A stream socket may hand over less than asked for
                     * (a pasted text makes long packets): take all of it,
                     * or the rest would be read as the next packet. */
                    ssize_t got = 0;
                    while (got < klen) {
                        ssize_t r = read(g_observer_fd, kbuf + got, (size_t)(klen - got));
                        if (r <= 0) break;
                        got += r;
                    }
                    if (got > 0 && g_wins[g_cur] && g_wins[g_cur]->alive) {
                        g_wins[g_cur]->pristine = false;
                        pty_write_all(g_wins[g_cur], (const uint8_t *)kbuf, (size_t)got);
                    }
                }
            } else if (type == 'W') {
                /* Window switch */
                uint8_t idx;
                if (read(g_observer_fd, &idx, 1) == 1)
                    switch_to((int)idx);
            } else if (type == 'C') {
                /* Generic mux command from observer */
                uint8_t cmd, arg;
                if (read(g_observer_fd, &cmd, 1) == 1 &&
                    read(g_observer_fd, &arg, 1) == 1) {
                    InputEvent ev = { .cmd = (InputCmd)cmd, .arg = (int)arg };
                    handle_event(ev);
                }
            }
        }

        /* Clock tick */
        if (clock_remaining_ms <= 0) {
            redraw_status();
            clock_remaining_ms = ms_to_next_minute();
        }

        /* Output that was read to make room for a long write (a paste)
         * has not been shown yet. */
        if (g_pty_drained) {
            g_pty_drained = false;
            Window *w = g_wins[g_cur];
            if (w && w->alive && w->vt.cpr_requested) {
                w->vt.cpr_requested = false;
                char reply[32];
                int rlen = snprintf(reply, sizeof(reply), "\033[%d;%dR",
                                    w->vt.scr.cur_row + 1, w->vt.scr.cur_col + 1);
                (void)write(w->pty.master, reply, (size_t)rlen);
            }
            if (w && !g_scrollback_mode) {
                render_screen(&g_rs, &w->vt.scr);
                observer_push();
            }
        }

        /* PTY masters */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window *w = g_wins[i];
            if (!w || w->pty.master < 0 || !FD_ISSET(w->pty.master, &rfds)) continue;
            if (drain_pty(w) && i == g_cur) {
                if (w->vt.cpr_requested) {
                    w->vt.cpr_requested = false;
                    char reply[32];
                    int rlen = snprintf(reply, sizeof(reply), "\033[%d;%dR",
                                        w->vt.scr.cur_row + 1,
                                        w->vt.scr.cur_col + 1);
                    (void)write(w->pty.master, reply, (size_t)rlen);
                }
                if (!g_scrollback_mode) {
#ifdef X11_BACKEND
                    (void)x11_backend_selection_drop();   /* (the render below repaints) */
#endif
                    render_screen(&g_rs, &w->vt.scr);
                    observer_push();
                }
                /* Status bar is NOT redrawn on every PTY update - only on
                 * window switches, child death, and the minute clock tick. */
            }
            /*
             * EIO-based death detection: covers the reattach case (children
             * reparented to init, so SIGCHLD never fires for them) and also
             * any SIGCHLD/drain_pty race in the normal case.
             * drain_pty() sets alive=false on EIO; we act on it here before
             * the next select() would spin forever on the dead fd.
             */
            if (!w->alive && w->pty.master >= 0) {
#if REMAIN_ON_EXIT
                window_mark_dead(i);
                if (i == g_cur) render_screen(&g_rs, &w->vt.scr);
                redraw_status();
#else
                window_free(i);
                if (i == g_cur) {
                    int next = -1;
                    for (int j = 0; j < MAX_WINDOWS; j++)
                        if (g_wins[j] && g_wins[j]->alive) { next = j; break; }
                    if (next >= 0) switch_to(next);
                    else           g_quit = 1;
                } else {
                    redraw_status();
                }
#endif
            }
        }

#ifndef X11_BACKEND
        /* stdin */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            uint8_t buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0 && errno != EINTR) die("read stdin");
            if (n == 0) {
                /* Controlling terminal closed (not always delivered as a
                 * clean SIGHUP, e.g. some non-SSH connection methods) —
                 * treat exactly like SIGHUP: auto-detach instead of exiting. */
                auto_detach();   /* never returns */
            }
            for (ssize_t i = 0; i < n; i++) {
#if SCROLLBACK_ENABLED
                if (scrollback_feed(buf[i], i == n - 1)) continue;
#endif
                InputEvent ev = input_feed(buf[i]);
                /* If ESC resolved (next byte consumed it), cancel the timer */
                if (!input_esc_pending()) esc_remaining_ms = -1;
                handle_event(ev);
            }
        }
#endif
    }

    cleanup();
    return 0;
}
