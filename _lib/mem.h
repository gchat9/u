#ifndef _LIB_MEM_H
#define _LIB_MEM_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *ptr, int c, size_t n);
void *memchr(const void *ptr, int c, size_t n);
static void *memmem(const void *haystack, size_t haystacklen,
                    const void *needle,   size_t needlelen);

#ifdef EXPORT_IMPLEMENTATIONS

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dest, const void *src, size_t n) {
    if (dest == NULL || src == NULL) return dest;

    char *d = (char *)dest;
    const char *s = (const char *)src;

    while (n--) *d++ = *s++;

    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void *memmem(const void *haystack, size_t haystacklen, 
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

void *memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;
    unsigned char byte = (unsigned char)c;

    while (n--)
        *p++ = byte;

    return ptr;
}

void *memchr(const void *ptr, int c, size_t n)
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

#endif // EXPORT_IMPLEMENTATIONS

#endif // _LIB_MEM_H
