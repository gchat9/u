#ifndef _LIB_MEM_H
#define _LIB_MEM_H

#include <stddef.h>

#ifndef MEMCPY_EXTERN
static inline void *memcpy(void *dest, const void *src, size_t n);
#endif
#ifndef MEMSET_EXTERN
static inline void *memset(void *ptr, int c, size_t n);
#endif
static inline void *memchr(const void *ptr, int c, size_t n);
static inline void *memmove(void *dest, const void *src, size_t n);
static inline int memcmp(const void *a, const void *b, size_t n);
static inline void *memmem(const void *haystack, size_t haystacklen,
                    const void *needle,   size_t needlelen);

#ifdef EXPORT_IMPLEMENTATIONS

#ifndef MEMCPY_EXTERN
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void *memcpy(void *dest, const void *src, size_t n) {
    if (dest == NULL || src == NULL) return dest;

    char *d = (char *)dest;
    const char *s = (const char *)src;

    while (n--) *d++ = *s++;

    return dest;
}
#endif

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void *memmem(const void *haystack, size_t haystacklen, 
                    const void *needle,   size_t needlelen) 
{
    if (needlelen == 0) {
        return (void *)haystack;
    }
    if (needlelen > haystacklen) {
        return 0;
    }

    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;

    /* We only need to search up to the point where the needle could still fit */
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        size_t j;
        for (j = 0; j < needlelen; j++) {
            if (h[i + j] != n[j]) {
                break;
            }
        }
        /* If we reached the end of the needle, we found a match */
        if (j == needlelen) {
            return (void *)(h + i);
        }
    }

    return 0;
}

#ifndef MEMSET_EXTERN
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void *memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;
    unsigned char byte = (unsigned char)c;

    while (n--)
        *p++ = byte;

    return ptr;
}

#endif

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void *memchr(const void *ptr, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)ptr;
    unsigned char byte = (unsigned char)c;

    while (n--) {
        if (*p == byte)
            return (void *)p;
        p++;
    }

    return NULL;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static inline int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

#endif // EXPORT_IMPLEMENTATIONS

#endif // _LIB_MEM_H
