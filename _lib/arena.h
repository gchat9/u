#ifndef _LIB_ARENA_H
#define _LIB_ARENA_H

#include <stddef.h>

/* A single growable anonymous mapping.  The kernel may move it while
 * growing, so callers must use base after each arena_grow call. */
struct arena {
    void *base;
    size_t size;
};

static inline int arena_init(struct arena *a, size_t size)
{
    a->base = mmap(0, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a->base == MAP_FAILED) {
        a->size = 0;
        return 1;
    }
    a->size = size;
    return 0;
}

static inline int arena_grow(struct arena *a, size_t needed)
{
    size_t size;
    void *base;

    if (needed <= a->size) return 0;
    size = a->size + a->size / 2;
    if (size < needed || size < a->size) size = needed;

    base = mremap(a->base, a->size, size, MREMAP_MAYMOVE);
    if (base == MAP_FAILED) return 1;
    a->base = base;
    a->size = size;
    return 0;
}

static inline int arena_read_all(struct arena *a, int fd, size_t *used)
{
    long n;

    for (;;) {
        if (*used == a->size && arena_grow(a, *used + 1)) return 1;
        n = read(fd, (char *)a->base + *used, a->size - *used);
        if (n < 0) return 1;
        if (!n) return 0;
        *used += (size_t)n;
    }
}

static inline int arena_destroy(struct arena *a)
{
    if (a->base == MAP_FAILED) return 0;
    return munmap(a->base, a->size) < 0;
}

#endif
