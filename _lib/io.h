#ifndef _LIB_IO_H
#define _LIB_IO_H

#include <stddef.h>

static inline int write_all_fd(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        long n = write(fd, p, len);
        if (n <= 0) return 1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

#endif
