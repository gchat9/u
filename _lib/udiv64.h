/*
 * udiv64.h - 64-bit unsigned division that needs no libgcc.
 *
 * 32-bit targets have no libgcc in this freestanding build, and the compiler
 * turns a division by a constant into a multiplication only when it has a
 * wide enough multiply: otherwise it calls __aeabi_uldivmod (armhf) or
 * __udivdi3 (i386), which do not exist here. Use udiv64() for every 64-bit
 * division and remainder where the operands are not known to fit 32 bits.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* N / D, and with REM not NULL, N % D in *REM. D is not zero. 32-bit targets get a
 * shift-and-subtract loop (64 steps); elsewhere it is the compiler's own division. */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
#if __SIZEOF_POINTER__ == 4
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
#else
    if (rem) *rem = n % d;
    return n / d;
#endif
}
