#pragma once
#include <stddef.h>
#include <stdint.h>
//can't include sys/types.h - this redefines timespec
//#include <sys/types.h>
#include "types.h"
#include "errno.h"
#include "signal.h"
#include "poll.h"
#include "stat.h"
#include "termios.h"

/* The architecture syscall prototypes only need this forward declaration. */
struct sockaddr;

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

#define AT_FDCWD            -100
#define AT_REMOVEDIR        0x200   /* unlinkat: remove a directory */
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH       0x1000  /* statx: the file is the descriptor given */

// unistd.h - lseek()
#define SEEK_SET            0
#define SEEK_CUR            1
#define SEEK_END            2

#define GRND_NONBLOCK       0x0001

// mmap
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define MAP_PRIVATE         0x2
#define MAP_ANONYMOUS       0x20
#define MAP_FAILED          ((void *)-1)
#define MREMAP_MAYMOVE      0x1
#define MADV_DONTNEED       4
#define MADV_COLD          20
#define STDIN_FILENO        0
#define STDOUT_FILENO       1
#define STDERR_FILENO       2
#define F_GETFD             1
#define F_SETFD             2
#define FD_CLOEXEC          1
#define PR_SET_DUMPABLE     4

#if defined(__x86_64__)
  #include "../_arch/x86_64.h"
#elif defined(__i386__)
  #include "../_arch/i386.h"
#elif defined(__aarch64__)
  #include "../_arch/aarch64.h"
#elif defined(__arm__)
  #include "../_arch/arm.h"
#elif defined(__riscv) && __riscv_xlen == 64
  #include "../_arch/riscv64.h"
#else
    #error "Target architecture not supported"
#endif

/* memset and memcpy: the compiler calls them for large initialisations and copies,
 * and on some arches it leaves those calls external. The one definition of each
 * is in the TU that holds _start (EXPORT__START), so it is linked once. */
#ifdef EXPORT__START
#define MEMSET_EXTERN 1
#define MEMCPY_EXTERN 1
#endif
#include "../_lib/arena.h"
#include "../_lib/io.h"
#include "../_lib/mem.h"
#include "../_lib/net.h"
#include "../_lib/str.h"

/* fork(): a clone with nothing but SIGCHLD, as fork(2) is on every arch here
 * (no fork syscall on aarch64 and riscv64). */
static inline int fork(void) { return (int)clone(SIGCHLD, 0, 0, 0, 0); }

/* sigaction() for a plain handler: the kernel needs SA_RESTORER and a return
 * trampoline on the arches that have SIG_RESTORER (see _arch/). The trampolines
 * call rt_sigreturn, and i386 and arm only build the frame that goes with it when
 * SA_SIGINFO is set (without it the frame is the old one, which sigreturn unwinds,
 * and the process dies of SIGSEGV on the handler's return). The handler is
 * called with the two extra arguments then; it does not look at them. */
static inline long sigaction_(int sig, void (*handler)(int), unsigned long flags)
{
    struct sigaction sa = {0};
    sa._u._sa_handler = handler;
    sa.sa_flags = flags;
#ifdef SIG_RESTORER
    sa.sa_flags |= SA_RESTORER | SA_SIGINFO;
    sa.sa_restorer = SIG_RESTORER;
#endif
    return rt_sigaction(sig, &sa, 0, 8);
}

#ifdef EXPORT__START
/* The loops are not turned back into calls by gcc: hence the attribute. */
__attribute__((optimize("no-tree-loop-distribute-patterns"), used))
void *memset(void *ptr, int c, size_t n) {
    unsigned char *p = ptr;
    while (n--) *p++ = (unsigned char)c;
    return ptr;
}
__attribute__((optimize("no-tree-loop-distribute-patterns"), used))
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}
#endif
