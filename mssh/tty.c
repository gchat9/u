/*
 * tty.c -- mssh's terminal front-end for the user's own terminal: stdin and
 * stdout, raw mode, SIGWINCH.  See mssh.h for the interface; ../xmssh/tty.c
 * is the same thing for an X11 window.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "mssh.h"

void tty_open(const char *title) { (void)title; }

/*
 * A PTY is requested if wanted and stdout is a tty; if it is not (e.g.
 * piped), there is none.
 */
const char *tty_term(int want_pty) {
    if (!want_pty || !isatty(STDOUT_FILENO)) return NULL;
    const char *term = getenv("TERM");
    return term ? term : "xterm-256color";
}

int tty_size(uint32_t *cols, uint32_t *rows, uint32_t *xpix, uint32_t *ypix) {
    struct winsize ws = {0};
    int rc = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    *cols = ws.ws_col; *rows = ws.ws_row;
    *xpix = ws.ws_xpixel; *ypix = ws.ws_ypixel;
    return rc < 0 ? -1 : 0;
}

void tty_out(int is_err, const uint8_t *data, size_t n) {
    xwrite(is_err ? STDERR_FILENO : STDOUT_FILENO, data, n);
}

/* In raw mode (OPOST off) a bare '\n' would only move down a line. */
static volatile sig_atomic_t raw_mode_active = 0;

void tty_msg(const char *s) {
    while (*s) {
        size_t n = strcspn(s, "\n");
        noret_write(STDERR_FILENO, s, n);
        s += n;
        if (*s) { noret_write(STDERR_FILENO, raw_mode_active ? "\r\n" : "\n", raw_mode_active ? 2 : 1); s++; }
    }
}

ssize_t tty_ask(char *buf, size_t n) { return read(STDIN_FILENO, buf, n); }

void tty_exit(int code) { exit(code); }

/* ════════════════════════ Raw-mode terminal ══════════════════════════════ */

static struct termios saved_termios;

static void restore_terminal(void) {
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

/* ══════════════════════════ SIGWINCH handling ════════════════════════════
 *
 * The handler just sets a flag; the loop checks it each iteration and
 * sends a "window-change" channel request with fresh terminal dimensions.
 * Using a flag keeps the handler async-signal-safe.
 */
static volatile sig_atomic_t g_winch = 0;
static void handle_winch(int sig) { (void)sig; g_winch = 1; }

/* atexit() does not run when a signal kills us, which left the user's
 * terminal in raw mode (no echo, no line editing) after SIGTERM/SIGHUP.
 * Restore it (tcsetattr is async-signal-safe) and die of the same signal. */
static void fatal_signal(int sig) {
    restore_terminal();
    raise(sig);                    /* SA_RESETHAND: the default action now */
}

static void enter_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &saved_termios);
    atexit(restore_terminal);
    struct termios raw = saved_termios;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;
    struct sigaction sa = {0};
    sa.sa_handler = handle_winch;
    sa.sa_flags   = SA_RESTART;   /* don't interrupt slow syscalls */
    sigaction(SIGWINCH, &sa, NULL);

    struct sigaction fs = {0};
    fs.sa_handler = fatal_signal;
    fs.sa_flags   = SA_RESETHAND | SA_NODEFER;
    static const int fatal[] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
    for (size_t i = 0; i < sizeof fatal / sizeof *fatal; i++) sigaction(fatal[i], &fs, NULL);
}

/* ═════════════════════════ Main I/O loop ════════════════════════════════
 *
 * poll() on stdin and the TCP socket simultaneously.
 * stdin → CHANNEL_DATA → server
 * server → CHANNEL_DATA → stdout (type 0), EXTENDED_DATA → stderr (type 1)
 */
int tty_run(SSH *ssh, int got_pty, int interactive) {
    (void)interactive;
    if (got_pty) enter_raw_mode();

    for (;;) {
        struct pollfd fds[2];
        /* After EOF, stdin leaves the set (fd -1 is ignored by poll): merely
         * clearing .events is not enough, as POLLHUP is reported regardless
         * and would make this loop spin until the server finishes. */
        /* ... and while the server cannot take more (its window is used up),
         * the input stays in the kernel instead of piling up here. */
        fds[0].fd = (ssh->client_eof || !ssh_send_room(ssh)) ? -1 : STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = ssh->fd;       fds[1].events = POLLIN;

        int n = poll(fds, 2, -1);
        if (n < 0 && errno != EINTR) die("poll: %s", strerror(errno));
        if (g_winch) { g_winch = 0; ssh_window_change(ssh); }
        if (n <= 0) continue;

        /* ── stdin → server ── */
        /* A pipe at EOF reports POLLHUP and not POLLIN, so POLLHUP (and
         * POLLERR) must lead to the read() that returns 0 and sends the EOF:
         * "echo hi | mssh host cat" used to hang, spinning, forever. */
        if (fds[0].fd >= 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t buf[4096]; size_t room = ssh_send_room(ssh);
            ssize_t nr = read(STDIN_FILENO, buf, room < sizeof(buf) ? room : sizeof(buf));
            if (nr <= 0) ssh_send_eof(ssh);        /* stdin EOF */
            else         ssh_send_input(ssh, buf, (size_t)nr);
        }

        /* ── server → us ── */
        if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) && ssh_pump(ssh)) break;
    }
    return ssh->exit_status;
}
