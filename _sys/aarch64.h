////////////// macros

#define _sa0(n)                 register long x8 asm("x8") = (long)(n);
#define _sa1(n,a)               \
        _sa0(n)                 register long x0 asm("x0") = (long)(a);
#define _sa2(n,a,b)             \
        _sa1(n,a)               register long x1 asm("x1") = (long)(b);
#define _sa3(n,a,b,c)           \
        _sa2(n,a,b)             register long x2 asm("x2") = (long)(c);
#define _sa4(n,a,b,c,d)         \
        _sa3(n,a,b,c)           register long x3 asm("x3") = (long)(d);
#define _sa5(n,a,b,c,d,e)       \
        _sa4(n,a,b,c,d)         register long x4 asm("x4") = (long)(e);
#define _sa6(n,a,b,c,d,e,f)     \
        _sa5(n,a,b,c,d,e)       register long x5 asm("x5") = (long)(f);

#define _syscall0(n)        _sa0(n)                                   \
    register long x0 asm("x0");                                       \
    asm volatile( "svc #0" : "=r"(x0) : "r"(x8) : "memory" );         \
    return x0;

#define _syscall1(n,a)      _sa1(n,a)                                 \
    asm volatile( "svc #0" : "+r"(x0) : "r"(x8) : "memory" );         \
    return x0;

#define _syscall2(n,a,b)    _sa2(n,a,b)                               \
    asm volatile( "svc #0" : "+r"(x0)                                 \
        : "r"(x8), "r"(x1)                                            \
        : "memory" );                                                 \
    return x0;

#define _syscall3(n,a,b,c)  _sa3(n,a,b,c)                             \
    asm volatile( "svc #0" : "+r"(x0)                                 \
        : "r"(x8), "r"(x1), "r"(x2)                                   \
        : "memory" );                                                 \
    return x0;

#define _syscall4(n,a,b,c,d) _sa4(n,a,b,c,d)                          \
    asm volatile( "svc #0" : "+r"(x0)                                 \
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3)                          \
        : "memory" );                                                 \
    return x0;

#define _syscall5(n,a,b,c,d,e) _sa5(n,a,b,c,d,e)                      \
    asm volatile( "svc #0" : "+r"(x0)                                 \
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)                 \
        : "memory" );                                                 \
    return x0;

#define _syscall6(n,a,b,c,d,e,f) _sa6(n,a,b,c,d,e,f)                  \
    asm volatile( "svc #0" : "+r"(x0)                                 \
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)        \
        : "memory" );                                                 \
    return x0;

////////////// syscalls

static inline void exit(int code) {
    _sa1(93, code);
    asm volatile("svc #0" : : "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}

static inline long openat(int dirfd, const char *pathname, int flags, int mode)
    { _syscall4(56, dirfd, pathname, flags, mode); }

/* open does not exist on aarch64; express via openat with AT_FDCWD    */
#define open(pathname, flags, mode) openat(-100, pathname, flags, mode)

static inline long close(int fd)
    { _syscall1(57, fd); }

static inline long read(int fd, void *buf, size_t count)
    { _syscall3(63, fd, buf, count); }

static inline long write(int fd, const void *buf, size_t count)
    { _syscall3(64, fd, buf, count); }

static inline long ppoll(struct pollfd *fds, unsigned long nfds,
                         const struct timespec *ts, const sigset_t *sigmask)
    { _syscall4(73, fds, nfds, ts, sigmask); }

/*  poll(2) does not exist on aarch64; ppoll is the direct  */
/*              syscall. Negative timeout → NULL timespec (infinite).  */
static inline long
poll(struct pollfd *fds, unsigned long nfds, int timeout_ms)
{
    if (timeout_ms < 0)
        { _syscall4(73, fds, nfds, 0, 0); }
    struct timespec ts = { timeout_ms / 1000,
                           (long)(timeout_ms % 1000) * 1000000L };
    _syscall4(73, fds, nfds, &ts, 0);
}

static inline long nanosleep(const struct timespec *request, struct timespec *remain)
    { _syscall2(101, request, remain); }

static inline long clock_gettime(int clk_id, struct timespec *tp)
    { _syscall2(113, clk_id, tp); }

static inline long rt_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oact, size_t sigsetsize)
    { _syscall4(134, sig, act, oact, sigsetsize); }

static inline long getpid(void)
    { _syscall0(172); }

static inline long socket(int domain, int type, int protocol)
    { _syscall3(198, domain, type, protocol); }

static inline long connect(int sockfd, const struct sockaddr *addr, int addrlen)
    { _syscall3(203, sockfd, addr, addrlen); }

static inline long sendto(int sockfd, const void *buf, size_t len, int flags,
                          const struct sockaddr *dest_addr, int addrlen)
    { _syscall6(206, sockfd, buf, len, flags, dest_addr, addrlen); }

static inline long recvfrom(int sockfd, void *buf, size_t len, int flags,
                            struct sockaddr *src_addr, int *addrlen)
    { _syscall6(207, sockfd, buf, len, flags, src_addr, addrlen); }

/* recv does not exist on aarch64; express via recvfrom with addr=NULL */
#define recv(sockfd, buf, len, flags) recvfrom(sockfd, buf, len, flags, 0, 0)

static inline long execve(const char *pathname,
                          char *const argv[], char *const envp[])
    { _syscall3(221, pathname, argv, envp); }

static inline long getrandom(void *buf, size_t buflen, unsigned int flags)
    { _syscall3(278, buf, buflen, flags); }

////////////// main

#ifdef EXPORT__START
// prototype for main
__attribute__((noreturn)) static void main(int argc, char **argv);

// broken, requires assembly patching
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    register void *sp asm("sp");
    long argc = ((long *)sp)[0];
    char **argv = (char **)((long *)sp + 1);
    main(argc, argv);
    __builtin_unreachable();
}
//
#endif

// not compatible with inlining
#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    __asm__ volatile(
        "ldr x0, [sp]       \n"  // argc is at [sp]
        "add x1, sp, #8     \n"  // argv is at [sp + 8]
        "bl  main           \n"
        "brk #0             \n"  // should never reach here
        ::: "x0", "x1", "memory"
    );
    __builtin_unreachable();
}
#endif

#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    long argc;
    char **argv;

    __asm__ volatile (
        "ldr %x0, [sp]\n\t"
        "add %x1, sp, #8\n\t"
        "mov x29, #0\n\t"
        "mov x30, #0\n\t"
        : "=r" (argc), "=r" (argv)
        :
        : "memory", "x29", "x30"
    );
    main(argc, argv);
}
#endif
