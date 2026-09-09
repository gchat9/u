#include "../_sys/_main.h"

#define BUFSZ  4096
#define MAXOUT    8   // arbitrary cap on stack-held fds

// TODO: accept -i (ignore SIGINT), --output-error, other long option forms.

static int write_all(int fd, const char *buf, long n)
{
    long off = 0, w;
    while (off < n) {
        w = write(fd, buf + off, n - off);
        if (w <= 0) return 1;
        off += w;
    }
    return 0;
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    int fds[MAXOUT];
    int nfds = 0;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int status = 0;
    int end_of_opts = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!end_of_opts) {
            if (strcasecmp(arg, "--") == 0) { end_of_opts = 1; continue; }
            if (strcasecmp(arg, "-a") == 0 || strcasecmp(arg, "--append") == 0) {
                flags = O_WRONLY | O_CREAT | O_APPEND;
                continue;
            }
        }

        if (nfds >= MAXOUT) {
            write(2, "tee: too many output files\n",
                  sizeof("tee: too many output files\n") - 1);
            exit(1);
            __builtin_unreachable();
        }

        int fd = open(arg, flags, 0644);
        if (fd < 0) {
            write(2, "tee: cannot open '", sizeof("tee: cannot open '") - 1);
            write(2, arg, strlen(arg));
            write(2, "'\n", sizeof("'\n") - 1);
            status = 1;
            continue;
        }
        fds[nfds++] = fd;
    }

    char buf[BUFSZ];
    for (;;) {
        long n = read(0, buf, sizeof(buf));
        if (n < 0) { status = 1; break; }
        if (n == 0) break;

        if (write_all(1, buf, n))
            status = 1;

        for (int i = 0; i < nfds; i++) {
            if (write_all(fds[i], buf, n))
                status = 1;
        }
    }

    for (int i = 0; i < nfds; i++)
        close(fds[i]);

    exit(status);
    __builtin_unreachable();
}
