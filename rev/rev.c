#include "../_sys/_main.h"

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
    char *p;
    size_t used = 0;
    int status = 0;

    if (arena_init(&a, 65536) || read_files(argc, argv, &a, &used)) exit(1);
    p = a.base;
    for (size_t start = 0; start < used; ) {
        size_t end = start;
        while (end < used && p[end] != '\n') end++;
        size_t left = start, right = end;
        while (left < right) {
            right--;
            char c = p[left];
            p[left] = p[right];
            p[right] = c;
            left++;
        }
        start = end < used ? end + 1 : used;
    }
    status = write_all_fd(1, p, used);
    exit(status);
    __builtin_unreachable();
}
