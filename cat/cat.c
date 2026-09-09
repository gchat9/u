#include "../_sys/_main.h"

#define BUFSZ 8192

// TODO: accept options -n, -A, -s etc. Currently just concatenates raw bytes.

static int copy_fd_to_stdout(int fd)
{
    char buf[BUFSZ];
    long n, w, off;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n == 0) return 0;

        off = 0;
        while (off < n) {
            w = write(1, buf + off, n - off);
            if (w <= 0) {
                exit(1);
                __builtin_unreachable();
            }
            off += w;
        }
    }
}

static void warn_cannot_open(const char *path)
{
    write(2, "cat: cannot open '", sizeof("cat: cannot open '") - 1);
    write(2, path, strlen(path));
    write(2, "'\n", sizeof("'\n") - 1);
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    int status = 0;

    if (argc <= 1) {
        status = copy_fd_to_stdout(0);
        exit(status);
        __builtin_unreachable();
    }

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        int fd;

        if (path[0] == '-' && path[1] == '\0') {
            fd = 0;
        } else {
            fd = open(path, O_RDONLY, 0);
            if (fd < 0) {
                warn_cannot_open(path);
                status = 1;
                continue;
            }
        }

        if (copy_fd_to_stdout(fd))
            status = 1;

        if (fd != 0)
            close(fd);
    }

    exit(status);
    __builtin_unreachable();
}
