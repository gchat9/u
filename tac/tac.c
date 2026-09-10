#include "../_sys/_main.h"

struct line { size_t off, len; };

static int read_files(int argc, char **argv, struct arena *a, size_t *used)
{
    if (argc == 1) return arena_read_all(a, 0, used);
    for (int i = 1; i < argc; i++) {
        int fd = 0;
        if (argv[i][0] != '-' || argv[i][1]) {
            fd = open(argv[i], O_RDONLY, 0);
            if (fd < 0) return 1;
        }
        if (arena_read_all(a, fd, used)) return 1;
        if (fd) close(fd);
    }
    return 0;
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct arena a;
    struct line *lines;
    size_t used = 0, nlines = 0, start = 0, required;

    if (arena_init(&a, 65536) || read_files(argc, argv, &a, &used)) exit(1);
    for (size_t i = 0; i < used; i++) {
        if (((char *)a.base)[i] == '\n') {
            nlines++;
            start = i + 1;
        }
    }
    if (start < used) nlines++;
    if (nlines > ((size_t)-1) / sizeof(struct line) ||
        used > (size_t)-1 - nlines * sizeof(struct line)) exit(1);
    required = used + nlines * sizeof(struct line);
    if (arena_grow(&a, required)) exit(1);

    lines = (struct line *)((char *)a.base + used);
    start = 0;
    nlines = 0;
    for (size_t i = 0; i < used; i++) {
        if (((char *)a.base)[i] == '\n') {
            lines[nlines++] = (struct line){start, i + 1 - start};
            start = i + 1;
        }
    }
    if (start < used)
        lines[nlines++] = (struct line){start, used - start};

    int status = 0;
    while (nlines) {
        nlines--;
        if (write_all_fd(1, (char *)a.base + lines[nlines].off,
                         lines[nlines].len)) {
            status = 1;
            break;
        }
    }
    exit(status);
    __builtin_unreachable();
}
