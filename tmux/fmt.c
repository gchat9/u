#include "fmt.h"

#include <stdarg.h>

/* v in decimal at p: the digits of v / 10 first, then the last. */
int put_u(char *p, unsigned v)
{
    int n = v >= 10 ? put_u(p, v / 10) : 0;
    p[n] = (char)('0' + v % 10);
    return n + 1;
}

/* DEC(): the 11 bytes are the 10 digits of an unsigned of 32 bits and a NUL. */
char *dec_(char *buf, unsigned v)
{
    buf[put_u(buf, v)] = '\0';
    return buf;
}

size_t join_(char *d, size_t cap, const char *s, ...)
{
    va_list ap;
    size_t n = 0;
    va_start(ap, s);
    for (; s; s = va_arg(ap, const char *))
        while (*s && n + 1 < cap) d[n++] = *s++;
    va_end(ap);
    d[n] = '\0';
    return n;
}
