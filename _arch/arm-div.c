/* uldivmod.c
 *
 * Minimal __aeabi_uldivmod() implementation for ARMv7/armhf with 32-bit UDIV.
 *
 * EABI behaviour:
 *   r0:r1 = numerator
 *   r2:r3 = denominator
 *   returns:
 *     r0:r1 = quotient
 *     r2:r3 = remainder
 *
 * Division by zero is undefined in C. This implementation chooses:
 *   quotient  = 0
 *   remainder = numerator
 */

typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware unsigned 32-bit divide.
 * Denominator must be non-zero.
 */
static inline u32 udiv32(u32 a, u32 b)
{
    u32 q;
    __asm__ ("udiv %0, %1, %2" : "=r" (q) : "r" (a), "r" (b));
    return q;
}

/*
 * Process 32 low bits of a restoring division.
 *
 * Input:
 *   *rem_p: current remainder, must be < d
 *   d     : divisor, non-zero
 *   bits  : next 32 numerator bits, MSB first
 *
 * Output:
 *   *rem_p: final remainder
 *   return: 32-bit quotient bits
 */
static u32 divmod32_tail(u64 *rem_p, u64 d, u32 bits)
{
    u64 rem = *rem_p;
    u32 q = 0;
    u32 i, bit, carry;

    for (i = 0; i < 32; ++i) {
        bit = bits >> 31;
        bits <<= 1;

        /*
         * rem = (rem << 1) | bit
         *
         * rem is mathematically up to 65 bits before the subtraction.
         * The carry bit is the lost bit 64. If carry is set, the value is
         * certainly >= d, and modulo subtraction gives the correct low 64 bits.
         */
        carry = (u32)(rem >> 63);
        rem = (rem << 1) | bit;

        q <<= 1;

        if (carry || rem >= d) {
            rem -= d;
            q |= 1;
        }
    }

    *rem_p = rem;
    return q;
}

/*
 * C helper called by the asm wrapper below.
 *
 * Returns quotient. Stores remainder through rp.
 */
__attribute__((used, noinline))
u64 uldivmod64_impl(u64 n, u64 d, u64 *rp)
{
    u32 n1, n0, d1, d0;

    if (d == 0) {
        *rp = n;
        return 0;
    }

    if (n < d) {
        *rp = n;
        return 0;
    }

    n1 = (u32)(n >> 32);
    n0 = (u32)n;
    d1 = (u32)(d >> 32);
    d0 = (u32)d;

    if (d1 == 0) {
        /*
         * 64-bit / 32-bit.
         *
         * Let B = 2^32.
         *
         * n = n1*B + n0
         * q1 = n1 / d0
         * r1 = n1 % d0
         *
         * Then:
         * q = q1*B + ((r1*B + n0) / d0)
         */
        u32 q1 = udiv32(n1, d0);
        u32 r1 = n1 - q1 * d0;

        u32 q0;
        u64 rem;

        if (r1 == 0) {
            q0 = udiv32(n0, d0);
            rem = (u32)(n0 - q0 * d0);
        } else {
            rem = r1;
            q0 = divmod32_tail(&rem, d, n0);
        }

        *rp = rem;
        return ((u64)q1 << 32) | q0;
    }

    /*
     * 64-bit / 64-bit, denominator >= 2^32.
     *
     * In this case the quotient is guaranteed to fit in 32 bits.
     *
     * The high 32 quotient bits are zero, and after processing the high
     * numerator word the remainder is simply n1. Then process the low 32 bits.
     */
    {
        u64 rem = n1;
        u32 q = divmod32_tail(&rem, d, n0);

        *rp = rem;
        return q;
    }
}

#ifdef __cplusplus
}
#endif

/*
 * ABI wrapper.
 *
 * GCC-generated calls to __aeabi_uldivmod expect:
 *   quotient in r0:r1
 *   remainder in r2:r3
 *
 * The C helper returns quotient in r0:r1 and writes the remainder to memory.
 * This wrapper loads that remainder into r2:r3.
 */
__asm__ (
    ".syntax unified\n"
    ".thumb\n"
    ".text\n"
    ".align 2\n"
    ".global __aeabi_uldivmod\n"
    ".thumb_func\n"
    "__aeabi_uldivmod:\n"

    /* Keep stack 8-byte aligned and preserve r4. */
    "push {r4, lr}\n"

    /*
     * Reserve space:
     *   [sp+0]  : pointer argument for helper
     *   [sp+8]  : 64-bit remainder storage
     */
    "sub sp, sp, #16\n"

    /* Pass pointer to remainder storage as 5th argument, i.e. [sp]. */
    "mov r4, sp\n"
    "adds r4, r4, #8\n"
    "str r4, [sp]\n"

    /* r0:r1 = numerator, r2:r3 = denominator are already in place. */
    "bl uldivmod64_impl\n"

    /* Load remainder into r2:r3. */
    "ldrd r2, r3, [sp, #8]\n"

    "add sp, sp, #16\n"
    "pop {r4, pc}\n"
);
