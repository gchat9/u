#ifndef _LIB_STR_H
#define _LIB_STR_H

#include <stddef.h>

static inline size_t strlen(const char *s);
static inline size_t strlcpy(char *dst, const char *src, size_t dsize);
static inline int strcasecmp(const char *s1, const char *s2);
static inline size_t utoa(unsigned long v, char *buf);

#ifdef EXPORT_IMPLEMENTATIONS

static inline size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static inline size_t strlcpy(char *dst, const char *src, size_t dsize)
{
    const char *s = src;

    if (dsize != 0) {
        size_t nleft = dsize;

        /* Copy as many bytes as will fit, leaving room for NUL. */
        while (--nleft != 0) {
            if ((*dst++ = *s++) == '\0')
                return (size_t)(s - src - 1);
        }

        /* dsize was reached before the end of src: NUL-terminate. */
        *dst = '\0';
    }

    /* Advance s to the end of src so we can return strlen(src). */
    while (*s++)
        ;

    return (size_t)(s - src - 1);
}

static inline int strcasecmp(const char *s1, const char *s2) {
    int c1, c2;
    while (1) {
        c1 = *s1++;
        c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 |= 0x20;
        if (c2 >= 'A' && c2 <= 'Z') c2 |= 0x20;
        if (c1 != c2) return c1 - c2;
        if (c1 == '\0') return 0;
    }
}

/* Writes the decimal representation of v into buf (NOT NUL-terminated).
 * buf must be at least 20 bytes. Returns the number of bytes written. */
static inline size_t utoa(unsigned long v, char *buf)
{
    size_t n = 0;

    do {
        buf[n++] = (char)('0' + (v % 10));
    } while (v /= 10);

    for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
        char c = buf[i];
        buf[i] = buf[j];
        buf[j] = c;
    }

    return n;
}

#endif // EXPORT_IMPLEMENTATIONS

#endif // _LIB_STR_H
