/* Through sh, the shell gets PWD as the panel has it (not with symbolic links
 * resolved). $0 is the shell, $1 the directory. Every shell honours PWD. The
 * history of bash (HISTIGNORE) and zsh (HISTORY_IGNORE) is told not to keep
 * the line sh_where() types. */
#define SH_SCRIPT "PWD=$1; export PWD; HISTIGNORE='*PWD}*'; HISTORY_IGNORE='*PWD}*'; export HISTIGNORE HISTORY_IGNORE; exec \"$0\" -i"

/* The shell: $SHELL -i on a pseudo-terminal of its own, made the controlling
 * terminal of a new session, started in the active panel's directory.
 * Returns 0, or -1 if no pseudo-terminal could be had. */
static int sh_start(void)
{
    const char *sh = genv("SHELL");
    char *argv[6], pts[32];
    unsigned int num = 0;
    int zero = 0, n;
    long m, s, pid;

    if (!sh || !*sh) sh = "/bin/sh";
    m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
    if (m < 0) return -1;
    ioctl((int)m, TIOCSPTLCK, &zero);
    if (ioctl((int)m, TIOCGPTN, &num) < 0) {
        close((int)m);
        return -1;
    }
    n = cat(pts, "/dev/pts/");
    n += (int)utoa(num, pts + n);
    pts[n] = 0;
    s = open(pts, O_RDWR | O_NOCTTY, 0);
    if (s < 0) {
        close((int)m);
        return -1;
    }
    size_pty((int)m);
    argv[0] = (char *)"sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)SH_SCRIPT;
    argv[3] = (char *)sh;
    argv[4] = P[act].path;
    argv[5] = NULL;
    pid = fork_child();
    if (pid == 0) {
        setsid();
        ioctl((int)s, TIOCSCTTY, 0);
        dup2((int)s, 0);
        dup2((int)s, 1);
        dup2((int)s, 2);
        close((int)s);
        if (!P[act].rem) chdir(P[act].path);   /* a link's path is not one here */
        execve("/bin/sh", argv, envp);
        exit(127);
    }
    close((int)s);
    if (pid < 0) {
        close((int)m);
        return -1;
    }
    sh_fd = (int)m;
    sh_pid = pid;
    return 0;
}

/* Keys typed in the shell view go to the shell as they are. */
static void shell_in(const unsigned char *b, int len)
{
    for (int k = 0; k < len; k++)
        if (b[k] == '\r') cmd_hist = 1;     /* a command has been run: [^] */
    while (len > 0) {
        long w = write(sh_fd, b, (size_t)len);

        if (w == -EINTR) continue;
        if (w <= 0) return;                 /* gone: shell_out() ends mc */
        b += w;
        len -= (int)w;
    }
}

/* The shell's output goes to the terminal, which shows the normal screen
 * while the shell is shown. When the shell has ended, so does mc, as in mc. */
static void shell_out(void)
{
    long n = read(sh_fd, ob, sizeof ob);

    if (n == -EINTR) return;
    if (n <= 0) {
        tty_leave();
        exit(0);
    }
    on = (int)n;
    flush();
}

/* Wait for input from the terminal. While the shell is shown, its output is
 * copied to the terminal in the meantime. Returns 1 when the terminal has
 * input, 0 when a signal (SIGWINCH) interrupted the wait. */
static int wait_term(void)
{
    struct pollfd pf[2];

    for (;;) {
        long r;

        pf[0].fd = 0;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = sh_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        r = poll(pf, hidden ? 2 : 1, -1);
        if (r == -EINTR) return 0;
        if (r < 0) return 1;                /* read() below ends mc */
        if (hidden && pf[1].revents) shell_out();
        if (pf[0].revents) return 1;
    }
}

/* Ctrl+O leaves the shell only when it is at its prompt: its terminal's
 * foreground process group is the shell itself, not a job it started. */
static int sh_idle(void)
{
    int pg = -1;

    return sh_fd >= 0 && ioctl(sh_fd, TIOCGPGRP, &pg) == 0 && pg == (int)sh_pid;
}

static int same16(const unsigned char *a, const char *b)
{
    int k;

    for (k = 0; k < 16; k++)
        if (a[k] != (unsigned char)b[k]) return 0;
    return 1;
}

/* Ask the idle shell where it is, as the shell itself says: after Ctrl+U clears
 * the line, a line with a leading space (bash keeps such lines out of its
 * history) echoes $PWD between two random tokens of 16 letters, 64 bits. The
 * path is left in ob, NUL-terminated, and its length is returned; -1 if there
 * is no answer within 50 ms. Nothing of the exchange reaches the terminal. */
static long sh_where(void)
{
    unsigned char *b = (unsigned char *)ob;
    char cmd[48], *tok;
    struct pollfd pf;
    long got;
    int k, n = 0, i, j;

    k = cat(cmd, "\x15 echo ");
    tok = cmd + k;                      /* the first token is written where it is sent */
    if (getrandom(tok, 16, 0) != 16) return -1;
    for (i = 0; i < 16; i++) tok[i] = (tok[i] & 15) + 'A';
    k += 16;
    k += cat(cmd + k, "${PWD}");
    for (i = 0; i < 16; i++) cmd[k + i] = tok[i];
    k += 16;
    cmd[k++] = '\r';
    shell_in((const unsigned char *)cmd, k);
    for (;;) {
        pf.fd = sh_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, 50) <= 0) return -1;
        got = read(sh_fd, ob + n, sizeof ob - 1 - (size_t)n);
        if (got <= 0) return -1;
        n += (int)got;
        for (i = 0; i + 17 <= n; i++) {
            if (!same16(b + i, tok) || b[i + 16] != '/') continue;
            for (j = i + 17; j + 16 <= n; j++)
                if (same16(b + j, tok)) {
                    for (k = 0; k < j - i - 16; k++) ob[k] = ob[i + 16 + k];
                    ob[j - i - 16] = 0;
                    return j - i - 16;
                }
            break;                          /* the second token is not in yet */
        }
        if (n >= (int)sizeof ob - 1) return -1;
    }
}

/* Leaving the shell view: the panel goes to the directory the shell is in, if
 * that is not the one it has. Then the shell is ended: closing its
 * pseudo-terminal sends it SIGHUP, so bash saves its history and exits. It is
 * reaped here, and readline's modes are put back. */
static void sh_park(void)
{
    long n;
    int k;

    if (sh_fd < 0) return;
    n = sh_where();
    if (n > 0) {
        for (k = 0; ob[k] && ob[k] == P[act].path[k]; k++) ;
        if (ob[k] || P[act].path[k]) change_to(&P[act], ob, ob);
    }
    close(sh_fd);
    sh_fd = -1;
    wait4((int)sh_pid, NULL, 0, NULL);
    outs("\x1b[?1l\x1b[?2004l");
}

/* Ctrl+O: the shell is shown, or the panels again. Leaving is refused, with a
 * bell, while the shell runs a command. The normal screen is the terminal's own,
 * so the shell's output stays there. A new shell's prompt goes where the last
 * one was: that line is erased from its start first. */
static void toggle_hidden(void)
{
    if (hidden) {
        if (!sh_idle()) {
            outs("\x07");
            flush();
            return;
        }
        hidden = 0;
        rfull = 1;
        sh_park();
        rescan_changed();
        tty_enter();
        return;
    }
    if (sh_fd < 0 && sh_start() < 0) return;
    hidden = 1;
    rfull = 1;
    if (on_alt) {
        outs("\x1b[?1049l");
        on_alt = 0;
    }
    outs(sh_seen ? "\r\x1b[J" : "\r\n");  /* as mc: the first prompt on a new line */
    sh_seen = 1;
    flush();
}

/* run_child() flags: the file is searched for in PATH; and a child that fails
 * (exits with a non-zero status) leaves its output on the screen until a key is
 * pressed. */
#define RUN_SEARCH 1
#define RUN_PAUSE  2

/* Run FILE with ARGV, in directory CWD, and wait for it to exit. With RUN_SEARCH,
 * FILE is looked up along $PATH. mc's panels come back afterwards. */
static void run_child(const char *file, char *const argv[], const char *cwd, int flags, const char *echo)
{
    struct sigaction sa;
    long pid;
    int st = 0;

    tty_leave();
    if (echo) {                           /* a command from the command line, as a shell shows it */
        outs("\r\n");
        put_prompt();
        outs(echo);
        outs("\r\n");
        flush();
    }
    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = SIG_IGN;               /* mc itself does not stop on Ctrl+C */
    rt_sigaction(SIGINT, &sa, NULL, 8);
    rt_sigaction(SIGQUIT, &sa, NULL, 8);

    pid = fork_child();
    if (pid == 0) {
        if (cwd) chdir(cwd);
        if (flags & RUN_SEARCH) exec_tool(file, argv);
        else execve(file, argv, envp);
        exit(127);
    }
    if (pid > 0)
        while (wait4((int)pid, &st, 0, NULL) == -EINTR) ;
    if (echo) {                           /* and the prompt for the next command */
        outs("\r\n");
        put_prompt();
    }

    zero_block(&sa, sizeof sa);
    sa._u._sa_handler = SIG_DFL;
    rt_sigaction(SIGINT, &sa, NULL, 8);
    rt_sigaction(SIGQUIT, &sa, NULL, 8);
    if ((flags & RUN_PAUSE) && (st & 0x7f) == 0 && ((st >> 8) & 0xff) != 0) {
        char key;

        outs("\r\n[failed: press a key] ");
        flush();
        if (tty_ok) ioctl(0, TCSETS, &tty_raw);
        read(0, &key, 1);
    }
    rescan_changed();
    tty_enter();
}
