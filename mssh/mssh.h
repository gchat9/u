#pragma once
/*
 * mssh.h -- what the SSH client (mssh.c) and its terminal front-end share.
 *
 * The client is the protocol plus the flow of one session.  It knows nothing
 * about the terminal it runs in, which is a front-end linked with it:
 *
 *   mssh/tty.c    the user's own terminal: stdin, stdout, raw mode, SIGWINCH
 *   xmssh/tty.c   a window of its own, an X11 terminal emulator (_lib/xtty)
 *
 * Both implement the tty_* functions below; the client implements the
 * ssh_* ones they call.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sodium.h>

typedef struct {
    int fd;

    /*
     * chacha20-poly1305@openssh.com uses 64-byte keys per direction.
     * OpenSSH layout (cipher-chachapoly.c):
     *   key[0..31]  = K_main   → Poly1305 key generation (ic=0) + body encrypt (ic=1)
     *   key[32..63] = K_header → length field encryption (ic=0)
     * Both halves are derived from a single 64-byte derive_key() call (letters C/D).
     */
    uint8_t send_key[64], recv_key[64];
    uint32_t send_seq, recv_seq;
    int strict_kex;                  /* both sides use "strict KEX" (see do_kex) */
    int send_enc, recv_enc;          /* flags: encryption active per direction */

    uint8_t session_id[crypto_hash_sha256_BYTES];
    int     have_session_id;
    uint8_t host_pk[32];             /* the server's host key, once accepted: a re-key must show the same */

    /* Single channel state */
    uint32_t local_chan, remote_chan;
    uint32_t remote_window;          /* how much more the server will take from us */
    uint32_t remote_maxpkt;          /* largest CHANNEL_DATA the server accepts */
    uint32_t recv_pending;           /* received and shown, not yet given back as window */

    /* Input that did not fit in remote_window yet, sent as the window opens;
     * eof_pending: the EOF follows it. */
    uint8_t *outq; size_t outq_len;
    int      eof_pending;

    /* Saved verbatim payloads for exchange hash construction */
    char     client_ver[256], server_ver[256];
    uint8_t *client_kexinit; size_t client_kexinit_len;
    uint8_t *server_kexinit; size_t server_kexinit_len;

    /* Session state, see ssh_pump() */
    int client_eof, server_eof;      /* each side has sent EOF */
    int exit_status;                 /* of the remote command, once known */
} SSH;

/* ── client services, for the front-end ── */

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* The command line as main() got it, before anything was parsed: a front-end
 * that starts the client afresh (xmssh, on a keypress) execs it again. */
extern char **ssh_argv;
void err(const char *s);                                   /* a message for the user, see tty_msg */
void xwrite(int fd, const void *buf, size_t n);            /* write all, or die */
void noret_write(int fd, const void *b, size_t n);         /* write, ignoring failure */

void ssh_send_input(SSH *ssh, const uint8_t *data, size_t n);  /* terminal input → server; what
                                                            * the window does not allow yet is
                                                            * kept and sent later, never lost */
size_t ssh_send_room(const SSH *ssh);                      /* how much input the server takes now;
                                                            * 0: do not read the terminal until
                                                            * ssh_pump() has made room */
void ssh_send_eof(SSH *ssh);                               /* no more input */
void ssh_window_change(SSH *ssh);                          /* the terminal changed size */
int  ssh_pump(SSH *ssh);                                   /* handle one server packet (ssh->fd is
                                                            * readable); nonzero: session over */
int  ssh_keepalive(SSH *ssh);                              /* before each wait for input: sends the keepalive when
                                                            * due, ends the session (does not return) if the server
                                                            * has been silent too long; returns the wait's maximum,
                                                            * in ms, to pass to poll() */

/* ── the terminal front-end, in tty.c ── */

/* The client starts: set up the terminal (the X11 front-end opens its window
 * here, titled `title`, so everything after can be shown in it). */
void tty_open(const char *title);

/* Is the session to have a pty?  want_pty: it would normally (an interactive
 * shell, or -t).  Returns the $TERM to announce, or NULL for no pty. */
const char *tty_term(int want_pty);

/* The terminal's size in characters and pixels; -1 if it cannot tell (the
 * values are then zero or unchanged). */
int  tty_size(uint32_t *cols, uint32_t *rows, uint32_t *xpix, uint32_t *ypix);

/* Output of the session: stdout (is_err 0) or stderr (1) of the remote side. */
void tty_out(int is_err, const uint8_t *data, size_t n);

/* A message of ours for the user, outside the session: a prompt, a warning, a
 * fatal error.  Plain text, '\n' ends a line. */
void tty_msg(const char *s);

/* Read the answer to a prompt, as read(2) would: up to n bytes; <= 0 if there
 * will be none. */
ssize_t tty_ask(char *buf, size_t n);

/* End the program, after a failure (code != 0) or a refused host key. */
void tty_exit(int code) __attribute__((noreturn));

/* Run the session: pass the terminal's input to ssh_send_input() (and
 * ssh_send_eof() at its end), size changes to ssh_window_change(), and wait for
 * the server too: ssh_pump() when ssh->fd is readable, until it says the
 * session is over.  Each wait is poll() with the timeout ssh_keepalive() returns,
 * called just before it.  got_pty: a pty was requested; interactive: it is a
 * shell, not a command.  Returns the exit status (ssh->exit_status). */
int  tty_run(SSH *ssh, int got_pty, int interactive);
