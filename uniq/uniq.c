#include "../_sys/_main.h"

#define BUFSZ 4096
#define MAX_LINE 4096

// ASCII-only case-insensitive comparison
// Returns 0 if equal, non-zero otherwise
static int strequal_ic(const char *s1, const char *s2, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c1 = s1[i];
        unsigned char c2 = s2[i];

        // Convert to lowercase for comparison (ASCII only)
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';

        if (c1 != c2) return 1;
    }
    return 0;
}

static void parse_args(int argc, char **argv, int *opt_count, int *opt_ignore_case)
{
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] == '-') {
            for (int j = 1; arg[j] != '\0'; j++) {
                switch (arg[j]) {
                    case 'c':
                        *opt_count = 1;
                        break;
                    case 'i':
                        *opt_ignore_case = 1;
                        break;
                    default:
                        // Unknown option, ignore
                        break;
                }
            }
        }
    }
}

static void output_line(const char *line, size_t len, long count, int opt_count)
{
    if (opt_count) {
        char buf[21];
        char *p = buf + sizeof buf;
        unsigned long c = (unsigned long)count;
        int n = 0;

        /* trailing space */
        *--p = ' ';

        /* digits, generated backwards */
        do {
            *--p = (char)('0' + (c % 10));
            c /= 10;
            ++n;
        } while (c);

        /* pad with spaces to at least 7 bytes long */
        while (n++ < 7) {
            *--p = ' ';
        }

        write(1, p, (size_t)((buf + sizeof buf) - p));
    }
    write(1, line, len);
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    char buf[BUFSZ];
    char line[MAX_LINE];
    char prev_line[MAX_LINE];
    long count;
    size_t pos, line_len, prev_len;
    int first_line;
    int opt_count = 0;
    int opt_ignore_case = 0;
    long nread;

    parse_args(argc, argv, &opt_count, &opt_ignore_case);

    count = 0;
    first_line = 1;
    prev_len = 0;
    pos = 0;

    while ((nread = read(0, buf, BUFSZ)) > 0) {
        for (long i = 0; i < nread; i++) {
            char c = buf[i];
            if (c == '\n') {
                line_len = pos;

                if (first_line) {
                    for (size_t j = 0; j < line_len; j++) {
                        prev_line[j] = line[j];
                    }
                    prev_len = line_len;
                    count = 1;
                    first_line = 0;
                } else {
                    int match;
                    if (line_len != prev_len) {
                        match = 0;
                    } else if (opt_ignore_case) {
                        match = (strequal_ic(line, prev_line, line_len) == 0);
                    } else {
                        match = 1;
                        for (size_t j = 0; j < line_len; j++) {
                            if (line[j] != prev_line[j]) {
                                match = 0;
                                break;
                            }
                        }
                    }

                    if (match) {
                        count++;
                    } else {
                        output_line(prev_line, prev_len, count, opt_count);
                        write(1, "\n", 1);
                        for (size_t j = 0; j < line_len; j++) {
                            prev_line[j] = line[j];
                        }
                        prev_len = line_len;
                        count = 1;
                    }
                }
                pos = 0;
            } else if (pos < MAX_LINE - 1) {
                line[pos++] = c;
            }
        }
    }

    // Handle last line without newline
    if (pos > 0 || !first_line) {
        if (pos > 0) {
            line_len = pos;
            if (!first_line) {
                int match;
                if (line_len != prev_len) {
                    match = 0;
                } else if (opt_ignore_case) {
                    match = (strequal_ic(line, prev_line, line_len) == 0);
                } else {
                    match = 1;
                    for (size_t j = 0; j < line_len; j++) {
                        if (line[j] != prev_line[j]) {
                            match = 0;
                            break;
                        }
                    }
                }
                if (match) {
                    count++;
                } else {
                    output_line(prev_line, prev_len, count, opt_count);
                    write(1, "\n", 1);
                    for (size_t j = 0; j < line_len; j++) {
                        prev_line[j] = line[j];
                    }
                    prev_len = line_len;
                    count = 1;
                }
            } else {
                for (size_t j = 0; j < line_len; j++) {
                    prev_line[j] = line[j];
                }
                prev_len = line_len;
                count = 1;
            }
        }
        if (!first_line || pos > 0) {
            output_line(prev_line, prev_len, count, opt_count);
            write(1, "\n", 1);
        }
    }

    exit(0);
    __builtin_unreachable();
}
