#ifndef _LIB_STR_H
#define _LIB_STR_H

#include <stddef.h>

static size_t strlen(const char *s);
size_t strlcpy(char *dst, const char *src, size_t dsize);
int strcasecmp(const char *s1, const char *s2);

#ifdef EXPORT_IMPLEMENTATIONS

static size_t strlen(const char *s) {
    const char *p=s;
    while (*p++);
    return (p-s);
}

size_t strlcpy(char *dst, const char *src, size_t dsize)
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

int strcasecmp(const char *s1, const char *s2) {
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

#endif // EXPORT_IMPLEMENTATIONS

#endif // _LIB_STR_H
