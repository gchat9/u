#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pty.h>             /* openpty()            */
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

int pty_open(Pty *pty, int rows, int cols, char *const *argv)
{
    int master, slave;
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    if (openpty(&master, &slave, NULL, NULL, &ws) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }

    if (pid == 0) {
        /* ---- child ---- */
        close(master);

        /* Become session leader and set slave as controlling terminal */
        if (setsid() < 0) _exit(1);
        if (ioctl(slave, TIOCSCTTY, 0) < 0) _exit(1);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);

        /* Tell programs what terminal we claim to be */
        setenv("TERM", "xterm-256color", 1);

        if (argv) {
            execvp(argv[0], argv);
        } else {
            const char *shell = getenv("SHELL");
            if (!shell || shell[0] == '\0')
                shell = "/bin/bash";
            execl(shell, shell, (char *)NULL);
        }
        _exit(127);
    }

    /* ---- parent ---- */
    close(slave);

    /* Set master non-blocking so our read loop can drain without blocking.
     * Set FD_CLOEXEC so the master is not inherited by subsequently
     * exec'd children (other windows' shells).  Without this, forking a
     * new window would leave the old master open in the new child, which
     * prevents SIGHUP from reaching the old window's shell when we close
     * our copy of that master. */
    int flags = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);

    pty->master = master;
    pty->child  = pid;
    return 0;
}

void pty_close(Pty *pty)
{
    if (pty->master >= 0) {
        close(pty->master);
        pty->master = -1;
    }
}

int pty_resize(Pty *pty, int rows, int cols)
{
    struct winsize ws = {
        .ws_row    = (unsigned short)rows,
        .ws_col    = (unsigned short)cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    return ioctl(pty->master, TIOCSWINSZ, &ws);
}
