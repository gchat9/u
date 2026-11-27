/*
 * heap_test.c -- drives _lib/heap.h: random allocations, reallocations and frees
 * with their contents checked, the alignment, zeroing and the refusal of sizes
 * that cannot be had.  Freestanding, as the programs that use heap.h are; built
 * and run for every target that heap_test.py finds a compiler for (the 32-bit
 * ones among them).  Exit status 0 when all is well.
 */
#define EXPORT__START 1
#define EXPORT_IMPLEMENTATIONS 1
#include "../_sys/_.h"
#include "heap.h"

#define N 300
static unsigned char *blk[N];
static size_t len[N];
static unsigned char tag[N];
static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }

static void say(const char *s, unsigned long v) {
    char b[24]; write(1, s, strlen(s)); write(1, b, utoa(v, b)); write(1, "\n", 1);
}
static int intact(const unsigned char *p, size_t n, unsigned char t) {
    for (size_t k = 0; k < n; k++) if (p[k] != t) return 0;
    return 1;
}

__attribute__((noreturn)) static void main(int argc, char **argv) {
    (void)argc; (void)argv;
    int bad = 0;
    say("sizeof(size_t) ", sizeof(size_t));

    void *p = heap_alloc(100);
    if (!p) { say("FAIL: heap_alloc(100) is NULL, ", 0); exit(1); }
    heap_free(p);

    /* what can never be had is NULL, not a block too small (the sums wrap around) */
    if (heap_alloc((size_t)-1) || heap_alloc((size_t)-16) || heap_alloc((size_t)-4000) ||
        heap_alloc((size_t)-4096 - 16))
        { say("FAIL: an impossible size gave a block, ", 0); bad++; }
    if (heap_calloc((size_t)-1, 2) || heap_calloc(2, (size_t)-1))
        { say("FAIL: calloc overflow gave a block, ", 0); bad++; }

    for (int it = 0; it < 100000; it++) {
        int i = rnd() % N, op = rnd() % 3;
        size_t sz = (rnd() % 10 == 0 ? rnd() % 200000 : rnd() % 3000) + 1;
        if (blk[i] && !intact(blk[i], len[i], tag[i])) { say("FAIL: a block changed, op ", it); bad++; break; }
        if (op == 0 || !blk[i]) {
            heap_free(blk[i]);
            blk[i] = heap_calloc(1, sz); len[i] = sz;
            if (!blk[i]) { say("FAIL: out of memory at ", it); exit(2); }
            if (!intact(blk[i], sz, 0)) { say("FAIL: calloc not zeroed, op ", it); bad++; }
        } else if (op == 1) {
            unsigned char *q = heap_realloc(blk[i], sz);
            if (!q) { say("FAIL: realloc NULL at ", it); exit(3); }
            size_t keep = sz < len[i] ? sz : len[i];
            if (!intact(q, keep, tag[i])) { say("FAIL: realloc lost contents, op ", it); bad++; }
            blk[i] = q; len[i] = sz;
        } else {
            heap_free(blk[i]); blk[i] = 0; continue;
        }
        if ((uintptr_t)blk[i] % 16) { say("FAIL: misaligned block, op ", it); bad++; }
        tag[i] = (unsigned char)(rnd() | 1);
        memset(blk[i], tag[i], len[i]);
    }
    say(bad ? "FAILED: problems " : "ok, problems ", bad);
    exit(bad != 0);
    __builtin_unreachable();
}
