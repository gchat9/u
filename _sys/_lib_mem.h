#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
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

//////////////////// strings

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
