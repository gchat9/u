#ifndef _LIB_STR_H
#define _LIB_STR_H

#include <stddef.h>

static inline size_t strlen(const char *s);
static inline size_t strlcpy(char *dst, const char *src, size_t dsize);
static inline size_t strlcat(char *dst, const char *src, size_t dsize);
static inline int strcasecmp(const char *s1, const char *s2);
static inline size_t utoa(unsigned long v, char *buf);
static inline int strcmp(const char *s1, const char *s2);
static inline char *strchr(const char *s, int c);
static inline char *strstr(const char *hay, const char *needle);
static inline size_t strcspn(const char *s, const char *reject);
static inline char *strcpy(char *dst, const char *src);
static inline int isalnum(int c);
static inline int isdigit(int c);
static inline long strtol(const char *s, char **end);

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

static inline size_t strlcat(char *dst, const char *src, size_t dsize)
{
    char *d = dst;
    const char *s = src;
    size_t nleft = dsize;

    /* Find the end of dst and adjust bytes left but don't go past end. */
    while (nleft != 0 && *d != '\0') {
        d++;
        nleft--;
    }

    /* If no space left, just return what would have been copied. */
    if (nleft == 0)
        return dsize + strlen(src);

    /* Copy as many bytes as will fit. */
    while (--nleft != 0) {
        if ((*d++ = *s++) == '\0')
            return (size_t)(d - dst - 1);
    }

    /* No space left for NUL, terminate anyway. */
    *d = '\0';

    /* Return total length attempted to copy. */
    return (size_t)(d - dst - 1) + strlen(s) + 1;
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

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static inline char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

static inline char *strstr(const char *hay, const char *needle) {
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return *needle ? 0 : (char *)hay;
}

static inline size_t strcspn(const char *s, const char *reject) {
    const char *p = s;
    while (*p && !strchr(reject, *p)) p++;
    return (size_t)(p - s);
}

static inline char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isalnum(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Decimal only, with an optional sign: what the command line and the ports need.
 * A value out of the range of long is LONG_MAX or LONG_MIN, not a wrapped number
 * (long is 32 bits on i386 and armhf). */
static inline long strtol(const char *s, char **end) {
    const unsigned long max = (unsigned long)__LONG_MAX__;
    const char *p = s; int neg = 0; unsigned long v = 0; int over = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (!isdigit((unsigned char)*p)) { if (end) *end = (char *)s; return 0; }
    while (isdigit((unsigned char)*p)) {
        unsigned d = (unsigned)(*p++ - '0');
        if (v > (max - d) / 10) over = 1; else v = v * 10 + d;
    }
    if (end) *end = (char *)p;
    if (over) return neg ? -(long)max - 1 : (long)max;
    return neg ? -(long)v : (long)v;
}

#endif // EXPORT_IMPLEMENTATIONS

#endif // _LIB_STR_H
