#pragma once

#include <stddef.h>

/*
 * Text without printf.  What the program writes is a few strings and the odd
 * number, which needs no format: the strings are joined, and a number is its
 * digits.
 *
 *   put_u(p, n)               n in decimal at p, no NUL; its length.  For
 *                             the escape sequences, where the digits go into
 *                             a buffer that is being built.
 *   DEC(n)                    n as a string, in a buffer made right there
 *                             (n is an unsigned of 32 bits, not a size_t or
 *                             a negative number).
 *   join(d, cap, a, b, ...)   the strings to d, which has room for cap bytes
 *                             and a NUL: cut where it is full.  Returns the
 *                             length, as it is in d.
 *
 * join() takes from one to any number of const char *, and the compiler
 * checks that: the sizeof is not evaluated, and costs nothing.
 *
 * What the program has to say to the user outside the screen goes to stderr
 * the same way, with no FILE:
 *
 *   warn(a, b, ...)           the strings, one after another, as they are
 *                             (no newline is added, and none is cut)
 *   perr(msg)                 "msg: " and the reason of errno, and a newline:
 *                             perror(), with a prefix that is chosen
 */
int    put_u(char *p, unsigned v);
char  *dec_(char *buf, unsigned v);
size_t join_(char *d, size_t cap, const char *s, ...) __attribute__((sentinel));
void   warn_(const char *s, ...) __attribute__((sentinel));
void   perr(const char *msg);

#define STRS_OK_(...) ((void)sizeof((const char *const[]){ __VA_ARGS__ }))
#define DEC(n)             dec_((char[11]){0}, (unsigned)(n))
#define join(d, cap, ...)  (STRS_OK_(__VA_ARGS__), join_((d), (cap), __VA_ARGS__, NULL))
#define warn(...)          (STRS_OK_(__VA_ARGS__), warn_(__VA_ARGS__, NULL))
