#include "../_sys/_main.h"

/* One growable anonymous mapping holds both the input and its line index. */
#define INITIAL_SIZE    (64 * 1024)
#define MAX_INPUT_SIZE  (10 * 1024 * 1024)

struct line {
    uint32_t off;
    uint32_t len;
};

struct options {
    int reverse;
    int numeric;
    int fold_case;
    int ignore_blanks;
    int unique;
};

static int folded(unsigned char c, int fold_case)
{
    if (fold_case && c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c;
}

static int compare_text(const struct line *a, const struct line *b,
                        const char *input, const struct options *o)
{
    size_t i = 0, j = 0;
    const char *as = input + a->off;
    const char *bs = input + b->off;

    if (o->ignore_blanks) {
        while (i < a->len && (as[i] == ' ' || as[i] == '\t')) i++;
        while (j < b->len && (bs[j] == ' ' || bs[j] == '\t')) j++;
    }
    while (i < a->len && j < b->len) {
        int ca = folded((unsigned char)as[i], o->fold_case);
        int cb = folded((unsigned char)bs[j], o->fold_case);
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
        j++;
    }
    if (i != a->len || j != b->len)
        return i == a->len ? -1 : 1;
    return 0;
}

static int digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int compare_number(const struct line *a, const struct line *b,
                          const char *input)
{
    size_t i = 0, j = 0, as, bs, ae, be;
    const char *asrc = input + a->off;
    const char *bsrc = input + b->off;
    int aneg = 0, bneg = 0;

    while (i < a->len && (asrc[i] == ' ' || asrc[i] == '\t')) i++;
    while (j < b->len && (bsrc[j] == ' ' || bsrc[j] == '\t')) j++;
    if (i < a->len && (asrc[i] == '-' || asrc[i] == '+')) {
        aneg = asrc[i] == '-';
        i++;
    }
    if (j < b->len && (bsrc[j] == '-' || bsrc[j] == '+')) {
        bneg = bsrc[j] == '-';
        j++;
    }

    as = i;
    while (i < a->len && digit((unsigned char)asrc[i])) i++;
    ae = i;
    bs = j;
    while (j < b->len && digit((unsigned char)bsrc[j])) j++;
    be = j;

    while (as < ae && asrc[as] == '0') as++;
    while (bs < be && bsrc[bs] == '0') bs++;
    if (as == ae) aneg = 0;
    if (bs == be) bneg = 0;

    if (aneg != bneg) return aneg ? -1 : 1;
    if (ae - as != be - bs) {
        int result = ae - as < be - bs ? -1 : 1;
        return aneg ? -result : result;
    }
    for (size_t k = 0; k < ae - as; k++) {
        if (asrc[as + k] != bsrc[bs + k]) {
            int result = asrc[as + k] < bsrc[bs + k] ? -1 : 1;
            return aneg ? -result : result;
        }
    }
    return 0;
}

static int compare_lines(const struct line *a, const struct line *b,
                         const char *input, const struct options *o)
{
    int result;
    if (o->numeric) result = compare_number(a, b, input);
    else result = compare_text(a, b, input, o);
    return o->reverse ? -result : result;
}

/* Heapsort is in-place, has predictable O(n log n) time, and needs no
 * recursion or temporary allocation. */
static void sort_lines(struct line *lines, size_t n, const char *input,
                       const struct options *o)
{
    size_t root;

    for (root = n / 2; root; root--) {
        size_t r = root - 1;
        for (;;) {
            size_t child = r * 2 + 1;
            if (child >= n) break;
            if (child + 1 < n &&
                compare_lines(&lines[child], &lines[child + 1], input, o) < 0)
                child++;
            if (compare_lines(&lines[r], &lines[child], input, o) >= 0) break;
            struct line tmp = lines[r];
            lines[r] = lines[child];
            lines[child] = tmp;
            r = child;
        }
    }
    for (size_t end = n; end > 1;) {
        end--;
        struct line tmp = lines[0];
        lines[0] = lines[end];
        lines[end] = tmp;
        root = 0;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= end) break;
            if (child + 1 < end &&
                compare_lines(&lines[child], &lines[child + 1], input, o) < 0)
                child++;
            if (compare_lines(&lines[root], &lines[child], input, o) >= 0) break;
            tmp = lines[root];
            lines[root] = lines[child];
            lines[child] = tmp;
            root = child;
        }
    }
}

static int read_fd(int fd, struct arena *arena, size_t *used)
{
    char extra;
    size_t room;
    long n;

    for (;;) {
        if (*used == MAX_INPUT_SIZE) {
            n = read(fd, &extra, 1);
            return n == 0 ? 0 : 1;
        }
        if (*used == arena->size) {
            if (arena_grow(arena, *used + 1))
                return 1;
        }
        room = arena->size - *used;
        if (room > MAX_INPUT_SIZE - *used) room = MAX_INPUT_SIZE - *used;
        n = read(fd, (char *)arena->base + *used, room);
        if (n < 0) return 1;
        if (n == 0) return 0;
        *used += (size_t)n;
    }
}

static int sort_buffer(size_t used, const struct options *o, char *input,
                       struct line *lines, size_t max_lines)
{
    size_t nlines = 0, start = 0;

    for (size_t i = 0; i < used; i++) {
        if (input[i] == '\n') {
            if (nlines == max_lines) return 1;
            lines[nlines++] = (struct line){(uint32_t)start, (uint32_t)(i - start)};
            start = i + 1;
        }
    }
    if (start < used) {
        if (nlines == max_lines) return 1;
        lines[nlines++] = (struct line){(uint32_t)start, (uint32_t)(used - start)};
    }

    sort_lines(lines, nlines, input, o);
    for (size_t i = 0; i < nlines; i++) {
        if (o->unique && i && compare_lines(&lines[i - 1], &lines[i], input, o) == 0)
            continue;
        if (write_all_fd(1, input + lines[i].off, lines[i].len) ||
            write_all_fd(1, "\n", 1))
            return 1;
    }
    return 0;
}

static void parse_options(int argc, char **argv, struct options *o, int *first)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == '-') {
            if (a[2] == '\0') { i++; break; }
            if (strcasecmp(a, "--reverse") == 0) o->reverse = 1;
            else if (strcasecmp(a, "--numeric-sort") == 0) o->numeric = 1;
            else if (strcasecmp(a, "--ignore-case") == 0) o->fold_case = 1;
            else if (strcasecmp(a, "--ignore-leading-blanks") == 0) o->ignore_blanks = 1;
            else if (strcasecmp(a, "--unique") == 0) o->unique = 1;
            continue;
        }
        for (int j = 1; a[j]; j++) {
            if (a[j] == 'r') o->reverse = 1;
            else if (a[j] == 'n') o->numeric = 1;
            else if (a[j] == 'f') o->fold_case = 1;
            else if (a[j] == 'b') o->ignore_blanks = 1;
            else if (a[j] == 'u') o->unique = 1;
        }
    }
    *first = i;
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct options o = {0, 0, 0, 0, 0};
    struct arena arena;
    char *input;
    struct line *lines;
    size_t used = 0, nlines = 0, required;
    int first, status = 0;

    if (arena_init(&arena, INITIAL_SIZE)) exit(1);
    parse_options(argc, argv, &o, &first);

    if (first == argc) {
        status = read_fd(0, &arena, &used);
    } else {
        for (int i = first; i < argc; i++) {
            int fd = 0;
            if (argv[i][0] != '-' || argv[i][1] != '\0') {
                fd = open(argv[i], O_RDONLY, 0);
                if (fd < 0) {
                    write(2, "sort: cannot open '", sizeof("sort: cannot open '") - 1);
                    write(2, argv[i], strlen(argv[i]));
                    write(2, "'\n", 2);
                    status = 1;
                    continue;
                }
            }
            if (read_fd(fd, &arena, &used)) status = 1;
            if (fd != 0) close(fd);
        }
    }
    if (used > MAX_INPUT_SIZE) status = 1;
    if (!status) {
        input = arena.base;
        for (size_t i = 0; i < used; i++)
            if (input[i] == '\n') nlines++;
        if (used && input[used - 1] != '\n') nlines++;
        if (nlines > (size_t)-1 / sizeof(struct line) ||
            used > (size_t)-1 - nlines * sizeof(struct line)) {
            status = 1;
        } else {
            required = used + nlines * sizeof(struct line);
            if (arena_grow(&arena, required)) status = 1;
        }
    }
    if (!status) {
        input = arena.base;
        lines = (struct line *)((char *)arena.base + used);
        status = sort_buffer(used, &o, input, lines, nlines);
    }
    exit(status);
    __builtin_unreachable();
}
