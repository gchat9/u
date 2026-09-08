#include <stddef.h>
#include <stdint.h>
//can't include sys/types.h - this redefines timespec
//#include <sys/types.h>
#include "types.h"

#include "_lib_net.h"

//#define NULL ((void*)0)

#ifdef __SIZE_TYPE__
/* horrible kludge to make sure size_t and ssize_t are both long or both int */
#define unsigned signed
typedef __SIZE_TYPE__ ssize_t;
#undef unsigned
#else
typedef signed long ssize_t;            /* Used for a count of bytes or an error indication. */
#endif

typedef int32_t clockid_t;

struct timespec {
    long tv_sec;   // seconds
    long tv_nsec;  // nanoseconds
};

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1

// fcntl.h - open() and friends
#define O_RDONLY             00
#define O_WRONLY             01
#define O_RDWR               02
#define O_CREAT            0100 /* not fcntl */
#define O_EXCL             0200 /* not fcntl */
#define O_NOCTTY           0400 /* not fcntl */
#define O_TRUNC           01000 /* not fcntl */
#define O_APPEND          02000
#define O_NONBLOCK        04000
#define O_LARGEFILE     0100000
#define O_DIRECTORY     0200000 /* must be a directory */
#define O_NOFOLLOW      0400000 /* don't follow links */
#define O_NOATIME       01000000
#define O_CLOEXEC       02000000
#define O_SYNC          (O_DSYNC|04000000)
#define O_PATH          010000000
#define O_TMPFILE       020000000

#define GRND_NONBLOCK       0x0001

#if defined(__x86_64__)
  #include "x86_64.h"
#elif defined(__i386__)
  #include "i386.h"
#elif defined(__aarch64__)
  #include "aarch64.h"
#elif defined(__arm__)
  #include "arm.h"
#else
    #error "Target architecture not supported"
#endif


///// lib_mem
void *memset(void *ptr, int c, size_t n);
void *memchr(const void *ptr, int c, size_t n);
static void *memmem(const void *haystack, size_t haystacklen, 
                    const void *needle,   size_t needlelen);
int strcasecmp(const char *s1, const char *s2);
