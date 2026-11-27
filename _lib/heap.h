#ifndef _LIB_HEAP_H
#define _LIB_HEAP_H

/*
 * heap_alloc(), heap_calloc(), heap_realloc(), heap_free() -- the memory of the
 * terminal emulator (vt.c, xterm_screen.c), without a C library.  A block has
 * a 16-byte header holding its size, so free() needs no size from the caller.
 * Blocks up to 64 KiB come from size classes (powers of two, from 32 bytes),
 * carved out of chunks of 256 KiB and reused through a list per class; larger
 * ones are mappings of their own, returned to the kernel when freed.  Each
 * translation unit has its own heap, and it only frees what it allocated.
 * A failed allocation gives NULL, as calloc() does.
 */

#include <stddef.h>
#include <stdint.h>

#define HEAP_HDR   16
#define HEAP_MIN_K  5                    /* 32 bytes */
#define HEAP_MAX_K 16                    /* 64 KiB: the largest size class */
#define HEAP_CHUNK (256u << 10)

struct heap_block { size_t cap; size_t pad[16 / sizeof(size_t) - 1]; };

struct heap_state {
    struct heap_block *free[HEAP_MAX_K + 1];   /* free blocks, linked through their payload */
    char *cur, *end;                           /* the chunk being carved */
};

static inline void *heap_alloc(size_t n);
static inline void *heap_calloc(size_t count, size_t size);
static inline void *heap_realloc(void *p, size_t n);
static inline void heap_free(void *p);

#ifdef EXPORT_IMPLEMENTATIONS

static inline struct heap_state *heap_state(void) {
    static struct heap_state h;
    return &h;
}

static inline void *heap_map(size_t n) {
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (uintptr_t)p > (uintptr_t)-4096 ? 0 : p;
}

static inline void *heap_alloc(size_t n) {
    struct heap_state *h = heap_state();
    struct heap_block *b;
    /* The header and the page rounding of a mapping must not wrap around: size_t
     * is 32 bits on i386 and armhf, where a wrapped size is a block that is too
     * small, and a fixed limit such as 1 << 40 is not even a valid shift. */
    if (n > (size_t)-1 - HEAP_HDR - 4095) return 0;
    size_t need = n + HEAP_HDR;
    if (need <= ((size_t)1 << HEAP_MAX_K)) {
        int k = HEAP_MIN_K;
        while (((size_t)1 << k) < need) k++;
        size_t sz = (size_t)1 << k;
        if (h->free[k]) {
            b = h->free[k];
            h->free[k] = *(struct heap_block **)(b + 1);
        } else {
            if ((size_t)(h->end - h->cur) < sz) {
                char *c = heap_map(HEAP_CHUNK);
                if (!c) return 0;
                h->cur = c; h->end = c + HEAP_CHUNK;
            }
            b = (struct heap_block *)h->cur;
            h->cur += sz;
        }
        b->cap = sz;
    } else {
        size_t sz = (need + 4095) & ~(size_t)4095;
        if (!(b = heap_map(sz))) return 0;
        b->cap = sz;
    }
    return b + 1;
}

static inline void *heap_calloc(size_t count, size_t size) {
    if (size && count > ((size_t)-1) / size) return 0;
    void *p = heap_alloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

static inline void *heap_realloc(void *p, size_t n) {
    if (!p) return heap_alloc(n);
    struct heap_block *b = (struct heap_block *)p - 1;
    size_t have = b->cap - HEAP_HDR;
    if (n <= have) return p;
    void *q = heap_alloc(n);
    if (!q) return 0;
    memcpy(q, p, have);
    heap_free(p);
    return q;
}

static inline void heap_free(void *p) {
    if (!p) return;
    struct heap_block *b = (struct heap_block *)p - 1;
    if (b->cap > ((size_t)1 << HEAP_MAX_K)) { munmap(b, b->cap); return; }
    struct heap_state *h = heap_state();
    /* not __builtin_ctzl: without the Zbb extension riscv64 calls libgcc for it */
    int k = HEAP_MIN_K;
    while (((size_t)1 << k) < b->cap) k++;
    *(struct heap_block **)(b + 1) = h->free[k];
    h->free[k] = b;
}

#endif
#endif
