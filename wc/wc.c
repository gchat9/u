#include "../_sys/_main.h"

#define BUFSZ 8192

// TODO: accept -L (max line length), --files0-from, locale-aware char counts.
//
// -c/--bytes, -m/--chars, -w/--words, -l/--lines, and "--" are implemented.
// -m assumes well-formed UTF-8 and just counts non-continuation bytes: any
// byte that isn't of the form 10xxxxxx starts a new codepoint. Simple,
// compact, and correct for valid UTF-8 (which includes plain ASCII).

struct counts {
    unsigned long lines, words, chars, bytes;
};

static int count_fd(int fd, struct counts *c)
{
    char buf[BUFSZ];
    long n;
    int in_word = 0;

    c->lines = c->words = c->chars = c->bytes = 0;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n == 0) return 0;

        c->bytes += (unsigned long)n;

        for (long i = 0; i < n; i++) {
            unsigned char b = (unsigned char)buf[i];

            if ((b & 0xC0) != 0x80) c->chars++;

            if (b == '\n') c->lines++;

            if (b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\v' || b == '\f') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
}

static void put_field(unsigned long v, int *first)
{
    char num[24];
    size_t l = utoa(v, num);
    if (!*first) write(1, " ", 1);
    write(1, num, l);
    *first = 0;
}

// Fixed field order (lines, words, chars, bytes) regardless of the order
// the corresponding flags were given on the command line, matching GNU wc.
static void print_counts(const struct counts *c, int show_l, int show_w, int show_m,
                          int show_c, const char *name)
{
    int first = 1;

    if (show_l) put_field(c->lines, &first);
    if (show_w) put_field(c->words, &first);
    if (show_m) put_field(c->chars, &first);
    if (show_c) put_field(c->bytes, &first);

    if (name) {
        write(1, " ", 1);
        write(1, name, strlen(name));
    }
    write(1, "\n", 1);
}

static void warn_cannot_open(const char *path)
{
    write(2, "wc: cannot open '", sizeof("wc: cannot open '") - 1);
    write(2, path, strlen(path));
    write(2, "'\n", sizeof("'\n") - 1);
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    int show_l = 0, show_w = 0, show_m = 0, show_c = 0;
    int status = 0;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == '-') {
            if (a[2] == '\0') { i++; break; } // "--"
            if (strcasecmp(a, "--bytes") == 0) show_c = 1;
            else if (strcasecmp(a, "--chars") == 0) show_m = 1;
            else if (strcasecmp(a, "--words") == 0) show_w = 1;
            else if (strcasecmp(a, "--lines") == 0) show_l = 1;
            continue;
        }
        for (int j = 1; a[j]; j++) {
            if (a[j] == 'c') show_c = 1;
            else if (a[j] == 'm') show_m = 1;
            else if (a[j] == 'w') show_w = 1;
            else if (a[j] == 'l') show_l = 1;
        }
    }

    if (!show_l && !show_w && !show_m && !show_c)
        show_l = show_w = show_c = 1;   // default: newlines, words, bytes

    if (i >= argc) {
        struct counts c;
        if (count_fd(0, &c)) status = 1;
        print_counts(&c, show_l, show_w, show_m, show_c, NULL);
        exit(status);
        __builtin_unreachable();
    }

    int nfiles = 0;
    for (int k = i; k < argc; k++) nfiles++;

    struct counts total = {0, 0, 0, 0};

    for (; i < argc; i++) {
        const char *path = argv[i];
        int fd;
        struct counts c;

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

        if (count_fd(fd, &c)) status = 1;
        print_counts(&c, show_l, show_w, show_m, show_c, path);

        total.lines += c.lines;
        total.words += c.words;
        total.chars += c.chars;
        total.bytes += c.bytes;

        if (fd != 0) close(fd);
    }

    if (nfiles > 1)
        print_counts(&total, show_l, show_w, show_m, show_c, "total");

    exit(status);
    __builtin_unreachable();
}
