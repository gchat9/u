/*
 * tty.c -- mssh's terminal front-end for an X11 window with its own xterm
 * emulator (_lib/xtty).  See ../mssh/mssh.h for the interface; ../mssh/tty.c
 * is the same thing for the user's own terminal.
 *
 * The window is the terminal from the start: it opens before the connection
 * does, so prompts and errors appear in it, and it stays up after a failure
 * (or a command's output) until it is closed, or until a space is pressed,
 * which starts the client again with the same command line.  The session always has a pty: there
 * is no cooked-mode terminal behind this window to echo and edit for a remote
 * program that has none.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../mssh/mssh.h"
#include "../_lib/xtty.h"

static int opened;                       /* the window exists */

/* The one way to close the window: its screen is freed with it, so nothing
 * may draw after this.  Every other use below checks opened first. */
static void window_close(void) {
    if (opened) { opened = 0; xtty_close(); }
}

void tty_open(const char *title) {
    if (xtty_open(title)) die("cannot open the X11 window ($DISPLAY, /etc/font.bfnt)");
    opened = 1;
}

const char *tty_term(int want_pty) { (void)want_pty; return "xterm-256color"; }

int tty_size(uint32_t *cols, uint32_t *rows, uint32_t *xpix, uint32_t *ypix) {
    int r = 24, c = 80;
    if (opened) xtty_size(&r, &c);
    *cols = (uint32_t)c; *rows = (uint32_t)r; *xpix = *ypix = 0;
    return 0;
}

void tty_out(int is_err, const uint8_t *data, size_t n) {
    (void)is_err;
    if (opened) xtty_write(data, n);      /* the window closed by the user: the output has nowhere to go */
}

/* Our own text, unlike the session's, has no pty to turn LF into CR LF. */
void tty_msg(const char *s) {
    if (!opened) { noret_write(STDERR_FILENO, s, strlen(s)); return; }
    for (; *s; s++) {
        if (*s == '\n') xtty_write("\r", 1);
        xtty_write(s, 1);
    }
    xtty_flush();
}

/* ── Waiting in the window: for a line of input, or for it to be closed ── */

static char   ans[16];
static size_t ans_n;
static int    ans_done;

/* Typed text, with the echo and backspace a cooked terminal would give. */
static void ask_input(const uint8_t *b, size_t n, void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\r' || b[i] == '\n') ans_done = 1;
        else if ((b[i] == 0x7f || b[i] == 8) && ans_n) { ans_n--; xtty_write("\b \b", 3); }
        else if (b[i] >= 0x20 && b[i] < 0x7f && ans_n < sizeof(ans)) { ans[ans_n++] = (char)b[i]; xtty_write(&b[i], 1); }
    }
}

/* A keypress comes as a chunk of exactly one byte; a paste or a mouse report
 * is longer, so only a lone space counts. */
static int space_pressed;
static void space_input(const uint8_t *b, size_t n, void *ctx) {
    (void)ctx;
    if (n == 1 && b[0] == ' ') space_pressed = 1;
}

/* Starts the client afresh with the command line it was given.  The window is
 * closed, and the process becomes a new copy of itself, which does everything
 * from the start: the host key check, the key, the connection, the session.
 * The connection is closed by exec (it is close-on-exec), and nothing of the
 * old session survives in the new image.  Returns only if the exec failed. */
static void reconnect(void) {
    window_close();
    execv("/proc/self/exe", ssh_argv);
    noret_write(STDERR_FILENO, "xmssh: cannot start again\n", strlen("xmssh: cannot start again\n"));
    _exit(1);
}

/* With the window up after the session (or a failure): waits until the window
 * is closed, or a space is pressed, which reconnects. */
static void wait_for_close_or_space(void) {
    space_pressed = 0;
    while (!xtty_closed()) {
        xtty_poll(1000, space_input, NULL);
        if (space_pressed) reconnect();
    }
}

ssize_t tty_ask(char *buf, size_t n) {
    if (!opened) return -1;
    ans_n = 0; ans_done = 0;
    while (!ans_done && !xtty_closed()) xtty_poll(1000, ask_input, NULL);
    if (!ans_done) return -1;
    xtty_write("\r\n", 2);
    if (n > ans_n) n = ans_n;
    memcpy(buf, ans, n);
    return (ssize_t)n;
}

/* Failing with the window up: leave what it shows to be read, and offer to
 * start again. */
void tty_exit(int code) {
    if (opened && code) {
        const char *m = "\r\n[press space to reconnect, or close the window]";
        xtty_write(m, strlen(m));
        wait_for_close_or_space();
    }
    window_close();
    exit(code);
}

/* ═════════════════════════ Main I/O loop ════════════════════════════════
 *
 * poll() on the window's X connection and the TCP socket.  xtty_poll()
 * handles the X side and calls on_input() with keys, mouse reports and pastes;
 * server output has been given to the window (tty_out) by the time we poll
 * again.
 */
static void on_input(const uint8_t *b, size_t n, void *ssh) { ssh_send_input(ssh, b, n); }

int tty_run(SSH *ssh, int got_pty, int interactive) {
    (void)got_pty;
    int closed = 0;                      /* the window was closed under us */

    for (;;) {
        if (opened) xtty_flush();
        struct pollfd fds[2];
        fds[0].fd = closed ? -1 : xtty_fd();  fds[0].events = POLLIN;
        fds[1].fd = ssh->fd;                  fds[1].events = POLLIN;

        if (poll(fds, 2, ssh_keepalive(ssh)) < 0 && errno != EINTR) die("poll: %s", strerror(errno));

        if (!closed && fds[0].revents) {
            xtty_poll(0, on_input, ssh);
            if (xtty_resized()) ssh_window_change(ssh);
            if (xtty_closed()) {         /* hang up; the server's CLOSE ends the session */
                window_close(); closed = 1;
                if (!ssh->client_eof) ssh_send_eof(ssh);
            }
        }
        if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) && ssh_pump(ssh)) break;
    }

    if (!closed) {
        if (!interactive) {              /* a command's output must stay readable */
            char m[96];
            snprintf(m, sizeof m, "\r\n[exit status %d - press space to reconnect, or close the window]",
                     ssh->exit_status);
            xtty_write(m, strlen(m));
            wait_for_close_or_space();
        }
        window_close();
    }
    return ssh->exit_status;
}
