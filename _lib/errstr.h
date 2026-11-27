#ifndef _LIB_ERRSTR_H
#define _LIB_ERRSTR_H

#include <stddef.h>
#include "str.h"

/*
 * errstr() -- the text of a Linux errno, as strerror() words it (glibc's text),
 * for the errors a client meets.  Packed: each entry is the errno as one byte,
 * the text, and a NUL; the table is a string, with no pointers and no padding.
 * Any other number gives "Unknown error".
 */
static inline const char *errstr(int e);

#ifdef EXPORT_IMPLEMENTATIONS

static inline const char *errstr(int e)
{
    static const char tab[] =
        "\1Operation not permitted\0"
        "\2No such file or directory\0"
        "\5Input/output error\0"
        "\11Bad file descriptor\0"
        "\13Resource temporarily unavailable\0"
        "\14Cannot allocate memory\0"
        "\15Permission denied\0"
        "\20Device or resource busy\0"
        "\21File exists\0"
        "\24Not a directory\0"
        "\25Is a directory\0"
        "\26Invalid argument\0"
        "\27Too many open files in system\0"
        "\30Too many open files\0"
        "\34No space left on device\0"
        "\36Read-only file system\0"
        "\40Broken pipe\0"
        "\50Too many levels of symbolic links\0"
        "\130Socket operation on non-socket\0"
        "\141Address family not supported by protocol\0"
        "\144Network is down\0"
        "\145Network is unreachable\0"
        "\150Connection reset by peer\0"
        "\153Transport endpoint is not connected\0"
        "\156Connection timed out\0"
        "\157Connection refused\0"
        "\161No route to host\0"
        "\163Operation now in progress\0"
        "\0";
    for (const char *p = tab; *p; p += 2 + strlen(p + 1))
        if ((unsigned char)*p == e) return p + 1;
    return "Unknown error";
}

#endif
#endif
