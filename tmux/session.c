/* session.c — detach/attach over a Unix domain socket
 *
 * Protocol (all multi-byte integers are native-endian; attach and detach
 * always use the same binary on the same machine):
 *
 *   Header (7 bytes):
 *     uint32_t  magic    0x55544D58  ('U','T','M','X')
 *     uint8_t   version  PROTO_VERSION
 *     uint8_t   cur_win  active window index
 *     uint8_t   nfds     number of PTY master fds (for SCM_RIGHTS)
 *
 *   Per slot (MAX_WINDOWS iterations, one byte even for empty slots):
 *     uint8_t   exists   0 = empty, skip; 1 = window present
 *     --- if exists:
 *     uint8_t   alive
 *     uint16_t  argc     number of argv strings (0 = default shell)
 *       for each arg:
 *         uint16_t  len
 *         char      str[len]   (not NUL-terminated on wire)
 *     vt_serialize() output   (see vt.c for format)
 *
 *   One SCM_RIGHTS message carrying nfds PTY master fds (in slot order,
 *   alive windows only).
 *
 *   Client sends 1-byte ACK (0x06) to signal successful receipt.
 *   Daemon unlinks socket and exits.
 */

#include "session.h"
#include "vt.h"
#include "pty.h"
#include "render.h"
#include "status.h"
#include "window.h"
#include "input.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define PROTO_MAGIC   0x55544D58u
#define PROTO_VERSION 1u

/* ------------------------------------------------------------------ */
/* I/O helpers                                                          */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = ECONNRESET; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SCM_RIGHTS helpers                                                   */
/* ------------------------------------------------------------------ */

static int send_fds(int sock, int *fds, int n)
{
    char dummy = 0;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    char cmsgbuf[CMSG_SPACE(MAX_WINDOWS * sizeof(int))];
    size_t clen = CMSG_SPACE((size_t)n * sizeof(int));
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsgbuf,
        .msg_controllen = clen,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN((size_t)n * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, (size_t)n * sizeof(int));

    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fds(int sock, int *fds, int n)
{
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    char cmsgbuf[CMSG_SPACE(MAX_WINDOWS * sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };
    if (recvmsg(sock, &msg, 0) < 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        errno = EPROTO;
        return -1;
    }
    int got = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (got != n) { errno = EPROTO; return -1; }

    memcpy(fds, CMSG_DATA(cmsg), (size_t)n * sizeof(int));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Protocol: send                                                       */
/* ------------------------------------------------------------------ */

static int send_session(int fd, Window *wins[], int cur)
{
    /* Build PTY fd list (alive windows, in slot order) */
    int pty_fds[MAX_WINDOWS];
    int nfds = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i] && wins[i]->alive && wins[i]->pty.master >= 0)
            pty_fds[nfds++] = wins[i]->pty.master;

    uint32_t magic = PROTO_MAGIC;
    uint8_t  ver   = PROTO_VERSION;
    uint8_t  ucur  = (uint8_t)cur;
    uint8_t  unfds = (uint8_t)nfds;

    if (write_all(fd, &magic, 4) < 0) return -1;
    if (write_all(fd, &ver,   1) < 0) return -1;
    if (write_all(fd, &ucur,  1) < 0) return -1;
    if (write_all(fd, &unfds, 1) < 0) return -1;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        uint8_t exists = wins[i] ? 1 : 0;
        if (write_all(fd, &exists, 1) < 0) return -1;
        if (!exists) continue;

        Window *w = wins[i];

        uint8_t alive = w->alive ? 1 : 0;
        if (write_all(fd, &alive, 1) < 0) return -1;

        /* Child PID — needed by the new process for /proc/<pid>/comm */
        int32_t cpid = (int32_t)w->pty.child;
        if (write_all(fd, &cpid, 4) < 0) return -1;

        /* argv */
        uint16_t argc = 0;
        if (w->argv) while (w->argv[argc]) argc++;
        if (write_all(fd, &argc, 2) < 0) return -1;
        for (uint16_t j = 0; j < argc; j++) {
            uint16_t slen = (uint16_t)strlen(w->argv[j]);
            if (write_all(fd, &slen,       2)    < 0) return -1;
            if (write_all(fd, w->argv[j], slen)  < 0) return -1;
        }

        /* VT state (includes cell grids) */
        if (vt_serialize(&w->vt, fd) < 0) return -1;
    }

    /* All PTY master fds in one SCM_RIGHTS message */
    if (nfds > 0 && send_fds(fd, pty_fds, nfds) < 0) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Protocol: receive                                                    */
/* ------------------------------------------------------------------ */

static int recv_session(int fd, Window *wins[], int *cur_out)
{
    uint32_t magic;
    uint8_t  ver, ucur, unfds;

    if (read_all(fd, &magic, 4) < 0) return -1;
    if (magic != PROTO_MAGIC) { errno = EPROTO; return -1; }
    if (read_all(fd, &ver,   1) < 0) return -1;
    if (ver != PROTO_VERSION)  { errno = EPROTO; return -1; }
    if (read_all(fd, &ucur,  1) < 0) return -1;
    if (read_all(fd, &unfds, 1) < 0) return -1;

    *cur_out = ucur;

    /* Track which slot each fd belongs to (alive windows, slot order) */
    int pty_slot[MAX_WINDOWS];
    int nfds_seen = 0;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        uint8_t exists;
        if (read_all(fd, &exists, 1) < 0) return -1;
        if (!exists) { wins[i] = NULL; continue; }

        Window *w = calloc(1, sizeof(Window));
        if (!w) return -1;
        wins[i]          = w;
        w->pty.master    = -1;  /* filled from SCM_RIGHTS below */
        w->pty.child     = 0;   /* unknown after re-attach; EIO detects death */

        uint8_t alive;
        if (read_all(fd, &alive, 1) < 0) return -1;
        w->alive = alive;

        int32_t cpid;
        if (read_all(fd, &cpid, 4) < 0) return -1;
        w->pty.child = (pid_t)cpid;

        /* argv */
        uint16_t argc;
        if (read_all(fd, &argc, 2) < 0) return -1;
        if (argc > 0) {
            w->argv = calloc((size_t)(argc + 1), sizeof(char *));
            if (!w->argv) return -1;
            for (uint16_t j = 0; j < argc; j++) {
                uint16_t slen;
                if (read_all(fd, &slen, 2) < 0) return -1;
                w->argv[j] = calloc(1, (size_t)slen + 1);
                if (!w->argv[j]) return -1;
                if (read_all(fd, w->argv[j], slen) < 0) return -1;
                /* NUL is already there from calloc */
            }
        }

        /* VT state */
        if (vt_deserialize(&w->vt, fd) < 0) return -1;

        if (w->alive) pty_slot[nfds_seen++] = i;
    }

    if (nfds_seen != (int)unfds) { errno = EPROTO; return -1; }

    /* Receive PTY master fds and assign them in slot order */
    if (unfds > 0) {
        int ptyfds[MAX_WINDOWS];
        if (recv_fds(fd, ptyfds, (int)unfds) < 0) return -1;
        for (int k = 0; k < (int)unfds; k++) {
            int slot = pty_slot[k];
            wins[slot]->pty.master = ptyfds[k];
            /* pty_open sets O_NONBLOCK; replicate that here */
            int fl = fcntl(ptyfds[k], F_GETFL);
            if (fl >= 0) fcntl(ptyfds[k], F_SETFL, fl | O_NONBLOCK);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Daemon loop                                                          */
/* ------------------------------------------------------------------ */

static void daemon_run(int sock, Window *wins[], int cur)
{
    /* Accept exactly one connection */
    int conn = accept(sock, NULL, NULL);
    if (conn >= 0) {
        uint8_t type = 0;
        if (read_all(conn, &type, 1) == 0 && type == '?') {
            uint8_t resp = 'D';
            write_all(conn, &resp, 1);
            uint8_t cmd = 0;
            if (read_all(conn, &cmd, 1) == 0 && cmd == SESSION_TYPE_ATTACH) {
                if (send_session(conn, wins, cur) == 0) {
                    uint8_t ack;
                    read_all(conn, &ack, 1);
                }
            }
        }
        close(conn);
    }

    close(sock);
    /* Do NOT unlink here: the new live instance that just received the
     * session will call session_listen() to rebind the path.  Unlinking
     * by path here would race against that and delete their fresh socket. */
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int session_listen(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("mux: socket"); return -1; }

    /* Close-on-exec so the socket is not inherited by child shells */
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MUX_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Check whether the socket is live before touching it.
     * Connect with a throw-away fd: success means a daemon is already
     * listening → refuse.  ECONNREFUSED / ENOENT means stale or absent
     * → safe to unlink and rebind. */
    {
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0) {
            if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(probe);
                close(sock);
                errno = EADDRINUSE;
                return -1;
            }
            close(probe);
        }
        unlink(MUX_SOCKET_PATH);   /* stale or absent — remove if present */
    }
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("mux: bind " MUX_SOCKET_PATH); close(sock); return -1;
    }
    if (listen(sock, 1) < 0) {
        perror("mux: listen"); close(sock); unlink(MUX_SOCKET_PATH); return -1;
    }
    return sock;
}

/*
 * Like session_listen but skips the liveness probe and unlinks
 * unconditionally.  Use when we know the previous owner (a daemon we
 * just detached from) is exiting and the socket file is stale-or-gone.
 */
int session_rebind(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("mux: socket"); return -1; }
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MUX_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(MUX_SOCKET_PATH);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("mux: bind " MUX_SOCKET_PATH); close(sock); return -1;
    }
    if (listen(sock, 1) < 0) {
        perror("mux: listen"); close(sock); unlink(MUX_SOCKET_PATH); return -1;
    }
    return sock;
}

void session_detach(int sock, Window *wins[], int cur)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("mux: fork");
        _exit(1);
    }

    if (pid > 0) {
        /* Parent: terminal already restored by caller.
         * Close our copy of the socket — the child (daemon) keeps it. */
        close(sock);
        _exit(0);
    }

    /* Child: become a headless daemon */
    setsid();

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }

    /* The socket was created without FD_CLOEXEC in the daemon — we need
     * it to survive.  Clear CLOEXEC now that we are the daemon process. */
    fcntl(sock, F_SETFD, 0);

    daemon_run(sock, wins, cur);
    /* never reached */
}

int session_attach(Window *wins[], int *cur_out,
                   int *new_session_sock_out, int *obs_fd_out)
{
    if (new_session_sock_out) *new_session_sock_out = -1;
    if (obs_fd_out)           *obs_fd_out           = -1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("mux: socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MUX_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("mux: connect"); close(sock); return -1;
    }

    /* Query: find out if we're talking to a daemon or a live instance */
    uint8_t query = '?';
    if (write_all(sock, &query, 1) < 0) {
        perror("mux: write"); close(sock); return -1;
    }
    uint8_t resp = 0;
    if (read_all(sock, &resp, 1) < 0) {
        perror("mux: read"); close(sock); return -1;
    }

    if (resp == 'L') {
        /*
         * Live instance: perform a role-reversal takeover.
         * Old-live sends us its session state + PTY fds + session socket.
         * We become the new live instance; old-live becomes observer.
         */
        uint8_t ttype = SESSION_TYPE_TAKEOVER;
        if (write_all(sock, &ttype, 1) < 0) { close(sock); return -1; }

        /* Receive window state and PTY master fds */
        if (recv_session(sock, wins, cur_out) < 0) {
            perror("mux: recv_session"); close(sock); return -1;
        }

        /* ACK */
        uint8_t ack = 0x06;
        write_all(sock, &ack, 1);

        /* Receive the session listening socket from old-live via SCM_RIGHTS */
        int new_session_sock = -1;
        if (recv_fds(sock, &new_session_sock, 1) < 0) {
            perror("mux: recv session socket"); close(sock); return -1;
        }
        fcntl(new_session_sock, F_SETFD, FD_CLOEXEC);

        /* Send our terminal size so old-live can enter observe at right dims */
        struct winsize ws = {0};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        int16_t my_rows = ws.ws_row > 0 ? ws.ws_row : 24;
        int16_t my_cols = ws.ws_col > 0 ? ws.ws_col : 80;
        write_all(sock, &my_rows, 2);
        write_all(sock, &my_cols, 2);

        /* Pass acquired fds back to caller via output params */
        *new_session_sock_out = new_session_sock;
        *obs_fd_out           = sock;

        return 0;
    }

    if (resp != 'D') { errno = EPROTO; close(sock); return -1; }

    uint8_t atype = SESSION_TYPE_ATTACH;
    if (write_all(sock, &atype, 1) < 0) {
        perror("mux: write"); close(sock); return -1;
    }

    if (recv_session(sock, wins, cur_out) < 0) {
        perror("mux: receive session"); close(sock); return -1;
    }

    /* ACK tells the daemon it can exit */
    uint8_t ack = 0x06;
    write_all(sock, &ack, 1);
    close(sock);
    return 0;
}

/* ObsHeader is defined in session.h */

/* Send one complete screen frame including status bar.
 * wins[] and cur_win supply metadata for the status bar. */
int session_observer_push(int obs_fd, const Screen *s,
                          Window *wins[], int cur_win)
{
    /* Collect window metadata */
    pid_t pids[MAX_WINDOWS]   = {0};
    bool  exists[MAX_WINDOWS] = {false};
    bool  alive[MAX_WINDOWS]  = {false};
    ObsHeader h;
    memset(&h, 0, sizeof(h));
    h.rows        = (int16_t)s->rows;
    h.cols        = (int16_t)s->cols;
    h.cur_row     = (int16_t)s->cur_row;
    h.cur_col     = (int16_t)s->cur_col;
    h.cur_visible = s->cur_visible ? 1 : 0;
    h.cur_win     = (uint8_t)cur_win;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        exists[i] = wins[i] != NULL;
        alive[i]  = wins[i] && wins[i]->alive;
        pids[i]   = (wins[i] && wins[i]->alive) ? wins[i]->pty.child : 0;
        if (exists[i]) h.win_exists_mask |= (uint16_t)(1u << i);
        if (alive[i])  h.win_alive_mask  |= (uint16_t)(1u << i);
        if (i < 10)    h.win_pids[i]      = (int32_t)pids[i];
    }

    /* Pre-render the status bar (uses s->cols for width) */
    char status_buf[4096];
    int slen = status_render(status_buf, sizeof(status_buf),
                             s->rows + 1, s->cols, cur_win,
                             pids, exists, alive);
    h.status_len = (slen > 0) ? (uint16_t)slen : 0;

    if (write_all(obs_fd, &h, sizeof(h)) < 0) return -1;

    size_t row_bytes = (size_t)s->cols * sizeof(Cell);
    for (int r = 0; r < s->rows; r++)
        if (write_all(obs_fd, s->cells[r], row_bytes) < 0) return -1;

    if (h.status_len > 0)
        if (write_all(obs_fd, status_buf, h.status_len) < 0) return -1;
    return 0;
}

int session_observer_init(int obs_fd, const Screen *s, int rows, int cols,
                          Window *wins[], int cur_win)
{
    /* Send the session's own terminal dimensions first */
    int16_t sr = (int16_t)rows, sc = (int16_t)cols;
    if (write_all(obs_fd, &sr, 2) < 0) return -1;
    if (write_all(obs_fd, &sc, 2) < 0) return -1;
    return session_observer_push(obs_fd, s, wins, cur_win);
}

/* ------------------------------------------------------------------ */
/* Observer client                                                      */
/* ------------------------------------------------------------------ */


/* Shared observe loop, entered after connection and size exchange.
 * sock must already have responded to '?' with 'L', sent 'O', and
 * received ses_rows/ses_cols. */


/* ------------------------------------------------------------------ */
/* Observe: screen frame wire format                                    */
/*                                                                      */
/* Server → client stream:                                              */
/*   [ObsHeader][Cell × rows × cols in logical row order] repeated     */
/*                                                                      */
/* ObsHeader.rows/cols are the SESSION dimensions; the observer clips   */
/* or pads its own terminal to fit.  A zero-rows header signals EOF.   */

static volatile sig_atomic_t obs_winch_flag = 0;
static void obs_on_winch(int sig) { (void)sig; obs_winch_flag = 1; }

/* Shared observe loop, entered after connection handshake and size receipt. */
int session_observe_fd(int sock, int ses_rows, int ses_cols)
{
    struct winsize ws = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int our_rows = ws.ws_row > 0 ? ws.ws_row : 24;
    int our_cols = ws.ws_col > 0 ? ws.ws_col : 80;

    int view_rows = (ses_rows < our_rows - 1 ? ses_rows : our_rows - 1);
    int view_cols = (ses_cols < our_cols      ? ses_cols : our_cols);

    RenderState rs;
    render_init(&rs, our_rows, our_cols);
    (void)write(STDOUT_FILENO, "\033[?1049h", 8);

    Cell **local = calloc((size_t)ses_rows, sizeof(Cell *));
    Cell  *data  = calloc((size_t)ses_rows * ses_cols, sizeof(Cell));
    if (!local || !data) {
        free(local); free(data);
        (void)write(STDOUT_FILENO, "\033[?1049l", 8);
        return -1;
    }
    for (int r = 0; r < ses_rows; r++)
        local[r] = data + r * ses_cols;

    Screen scr;
    memset(&scr, 0, sizeof(scr));
    scr.rows        = view_rows;
    scr.cols        = view_cols;
    scr.cur_visible = 1;

    struct termios raw, saved_termios;
    tcgetattr(STDIN_FILENO, &saved_termios);
    raw = saved_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    obs_winch_flag = 0;
    struct sigaction sa_winch = {0}, sa_old = {0};
    sa_winch.sa_handler = obs_on_winch;
    sigaction(SIGWINCH, &sa_winch, &sa_old);

    long esc_rem = -1;

    ObsHeader last_h;
    memset(&last_h, 0, sizeof(last_h));
    char last_status_buf[4096];
    int  last_status_len = 0;

    for (;;) {
        /* Handle observer terminal resize */
        if (obs_winch_flag) {
            obs_winch_flag = 0;
            struct winsize ws2 = {0};
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2);
            our_rows  = ws2.ws_row > 0 ? ws2.ws_row : 24;
            our_cols  = ws2.ws_col > 0 ? ws2.ws_col : 80;
            view_rows = (ses_rows < our_rows - 1 ? ses_rows : our_rows - 1);
            view_cols = (ses_cols < our_cols      ? ses_cols : our_cols);
            scr.rows  = view_rows;
            scr.cols  = view_cols;
            render_free(&rs);
            (void)write(STDOUT_FILENO, "\033[2J", 4);
            render_init(&rs, our_rows, our_cols);
            if (last_h.rows > 0) {
                scr.cells       = local;
                scr.cur_row     = last_h.cur_row < view_rows ? last_h.cur_row : view_rows - 1;
                scr.cur_col     = last_h.cur_col < view_cols ? last_h.cur_col : view_cols - 1;
                scr.cur_visible = last_h.cur_visible;
                render_screen(&rs, &scr);
                if (last_status_len > 0) {
                    (void)write(STDOUT_FILENO, last_status_buf, (size_t)last_status_len);
                    char mv[16];
                    int mvn = snprintf(mv, sizeof(mv), "\033[%d;%dH",
                                       scr.cur_row + 1, scr.cur_col + 1);
                    (void)write(STDOUT_FILENO, mv, (size_t)mvn);
                }
            }
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock,         &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;

        struct timeval tv, *tvp = NULL;
        if (esc_rem >= 0) {
            tv.tv_sec  = esc_rem / 1000;
            tv.tv_usec = (esc_rem % 1000) * 1000;
            tvp = &tv;
        }
        if (select(maxfd + 1, &rfds, NULL, NULL, tvp) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* ESC timeout */
        if (esc_rem >= 0 && !FD_ISSET(STDIN_FILENO, &rfds)) {
            InputEvent ev = input_flush_esc();
            if (ev.cmd == CMD_PASS_BYTE) {
                uint8_t pkt[3] = { 'K', 1, (uint8_t)ev.byte };
                if (write_all(sock, pkt, 3) < 0) break;
            }
            esc_rem = -1;
        }

        /* Keyboard input: run through mux FSM, send typed packets upstream */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char keybuf[256];
            ssize_t n = read(STDIN_FILENO, keybuf, sizeof(keybuf));
            if (n <= 0) break;
            for (ssize_t ki = 0; ki < n; ki++) {
                InputEvent ev = input_feed((uint8_t)keybuf[ki]);
                if (!input_esc_pending()) esc_rem = -1;
                else if (esc_rem < 0)     esc_rem = ESC_TIMEOUT_MS;

                uint8_t pkt[3];
                switch (ev.cmd) {
                case CMD_PASS_BYTE:
                    pkt[0] = 'K'; pkt[1] = 1; pkt[2] = (uint8_t)ev.byte;
                    if (write_all(sock, pkt, 3) < 0) goto done;
                    break;
                case CMD_PASS_TWO:
                    pkt[0] = 'K'; pkt[1] = 2;
                    if (write_all(sock, pkt, 2) < 0) goto done;
                    pkt[0] = (uint8_t)(ev.byte >> 8);
                    pkt[1] = (uint8_t)ev.byte;
                    if (write_all(sock, pkt, 2) < 0) goto done;
                    break;
                case CMD_SELECT_WINDOW:
                    pkt[0] = 'W'; pkt[1] = (uint8_t)ev.arg;
                    if (write_all(sock, pkt, 2) < 0) goto done;
                    break;
                case CMD_QUIT:
                case CMD_DETACH:
                    goto done;
                case CMD_NONE:
                    break;
                default:
                    pkt[0] = 'C'; pkt[1] = (uint8_t)ev.cmd; pkt[2] = (uint8_t)ev.arg;
                    if (write_all(sock, pkt, 3) < 0) goto done;
                    break;
                }
            }
        }

        /* Incoming frame */
        if (FD_ISSET(sock, &rfds)) {
            if (read_all(sock, &last_h, sizeof(last_h)) < 0 || last_h.rows == 0) break;
            if (last_h.rows == -1) {
                /* Takeover sentinel: main session is becoming a daemon.
                 * Exit observe loop so caller can re-attach. */
                sigaction(SIGWINCH, &sa_old, NULL);
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
                free(local); free(data);
                render_free(&rs);
                (void)write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
                return 1;
            }
            ObsHeader h = last_h;

            /* Session resized: reallocate and re-init */
            if (h.rows != ses_rows || h.cols != ses_cols) {
                free(local); free(data);
                ses_rows  = (int)h.rows;
                ses_cols  = (int)h.cols;
                view_rows = (ses_rows < our_rows - 1 ? ses_rows : our_rows - 1);
                view_cols = (ses_cols < our_cols      ? ses_cols : our_cols);
                local = calloc((size_t)ses_rows, sizeof(Cell *));
                data  = calloc((size_t)ses_rows * ses_cols, sizeof(Cell));
                if (!local || !data) goto done;
                for (int r = 0; r < ses_rows; r++)
                    local[r] = data + r * ses_cols;
                scr.rows = view_rows;
                scr.cols = view_cols;
                render_free(&rs);
                render_init(&rs, our_rows, our_cols);
            }

            size_t row_bytes = (size_t)ses_cols * sizeof(Cell);
            for (int r = 0; r < (int)h.rows; r++)
                if (read_all(sock, local[r], row_bytes) < 0) goto done;

            char status_buf[4096];
            uint16_t slen = h.status_len;
            if (slen > 0 && slen <= sizeof(status_buf)) {
                if (read_all(sock, status_buf, slen) < 0) goto done;
            } else { slen = 0; }

            last_status_len = (int)slen;
            if (slen > 0) memcpy(last_status_buf, status_buf, slen);

            scr.cells       = local;
            scr.cur_row     = h.cur_row < view_rows ? h.cur_row : view_rows - 1;
            scr.cur_col     = h.cur_col < view_cols ? h.cur_col : view_cols - 1;
            scr.cur_visible = h.cur_visible;
            render_screen(&rs, &scr);

            if (slen > 0) {
                (void)write(STDOUT_FILENO, status_buf, slen);
                char mv[16];
                int mvn = snprintf(mv, sizeof(mv), "\033[%d;%dH",
                                   scr.cur_row + 1, scr.cur_col + 1);
                (void)write(STDOUT_FILENO, mv, (size_t)mvn);
            }
        }
    }

done:
    sigaction(SIGWINCH, &sa_old, NULL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    free(local);
    free(data);
    render_free(&rs);
    (void)write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    return 0;
}


int session_observe(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("mux: socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MUX_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("mux: connect"); close(sock); return -1;
    }

    uint8_t q = '?';
    if (write_all(sock, &q, 1) < 0) { perror("mux: write"); close(sock); return -1; }
    uint8_t qr = 0;
    if (read_all(sock, &qr, 1) < 0 || qr != 'L') {
        fputs("mux observe: not a live session\n", stderr); close(sock); return -1;
    }

    uint8_t otype = SESSION_TYPE_OBSERVE;
    if (write_all(sock, &otype, 1) < 0) { close(sock); return -1; }

    int16_t sr, sc;
    if (read_all(sock, &sr, 2) < 0 || read_all(sock, &sc, 2) < 0) {
        fputs("mux observe: failed to receive session size\n", stderr);
        close(sock); return -1;
    }

    int ret = session_observe_fd(sock, (int)sr, (int)sc);
    close(sock);
    return ret;
}

/*
 * Called by the old-live instance when it receives SESSION_TYPE_TAKEOVER.
 * Sends full session state + PTY fds + session socket to the new main,
 * reads back the new main's terminal size.
 * Returns 0 on success; obs_fd_out is the open channel to observe over.
 */
int session_do_takeover(int conn, int session_sock,
                        Window *wins[], int cur,
                        int *obs_fd_out, int *rows_out, int *cols_out)
{
    if (send_session(conn, wins, cur) < 0) return -1;

    uint8_t ack = 0;
    read_all(conn, &ack, 1);

    if (send_fds(conn, &session_sock, 1) < 0) return -1;

    int16_t nr = 0, nc = 0;
    if (read_all(conn, &nr, 2) < 0 || read_all(conn, &nc, 2) < 0) return -1;

    *obs_fd_out = conn;
    *rows_out   = (int)nr;
    *cols_out   = (int)nc;
    return 0;
}

int session_become_observer(int conn, int rows, int cols)
{
    (void)write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    int ret = session_observe_fd(conn, rows, cols);
    close(conn);
    return ret;   /* 1 = takeover sentinel received */
}
