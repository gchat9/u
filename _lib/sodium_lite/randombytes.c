/*
 * randombytes.c -- randombytes_buf() of sodium_lite, from the kernel.  A file of
 * its own because it uses this repository's system call layer, which does not
 * go together with the libc headers the rest includes.
 */
#define EXPORT_IMPLEMENTATIONS 1         /* as xterm.c: _sys/_.h without libc headers */
#include "../../_sys/_.h"                /* getrandom() */
#include "sodium.h"

void randombytes_buf(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        long r = getrandom(p, n, 0);
        if (r == -4) continue;                       /* EINTR */
        if (r <= 0) __builtin_trap();                /* never go on with predictable "random" bytes */
        p += r; n -= (size_t)r;
    }
}
