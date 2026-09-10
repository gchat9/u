#define _sa0(n)                 register long r7 asm("r7") = (long)(n);
#define _sa1(n,a)               \
    _sa0(n)                     register long r0 asm("r0") = (long)(a);
#define _sa2(n,a,b)             \
    _sa1(n,a)                   register long r1 asm("r1") = (long)(b);
#define _sa3(n,a,b,c)           \
    _sa2(n,a,b)                 register long r2 asm("r2") = (long)(c);
#define _sa4(n,a,b,c,d)         \
    _sa3(n,a,b,c)               register long r3 asm("r3") = (long)(d);
#define _sa5(n,a,b,c,d,e)       \
    _sa4(n,a,b,c,d)             register long r4 asm("r4") = (long)(e);
#define _sa6(n,a,b,c,d,e,f)     \
    _sa5(n,a,b,c,d,e)           register long r5 asm("r5") = (long)(f);

#define _syscall0(n) _sa0(n)                                           \
    register long r0 asm("r0");                                        \
    asm volatile( "svc #0" : "=r"(r0)                                  \
        : "r"(r7)                                                      \
        : "memory" );                                                  \
    return r0;

#define _syscall1(n,a) _sa1(n,a)                                       \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7)                                                      \
        : "memory" );                                                  \
    return r0;

#define _syscall2(n,a,b) _sa2(n,a,b)                                   \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7), "r"(r1)                                             \
        : "memory" );                                                  \
    return r0;

#define _syscall3(n,a,b,c) _sa3(n,a,b,c)                               \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7), "r"(r1), "r"(r2)                                    \
        : "memory" );                                                  \
    return r0;

#define _syscall4(n,a,b,c,d) _sa4(n,a,b,c,d)                           \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7), "r"(r1), "r"(r2), "r"(r3)                           \
        : "memory" );                                                  \
    return r0;

#define _syscall5(n,a,b,c,d,e) _sa5(n,a,b,c,d,e)                       \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)                  \
        : "memory" );                                                  \
    return r0;

#define _syscall6(n,a,b,c,d,e,f) _sa6(n,a,b,c,d,e,f)                   \
    asm volatile( "svc #0" : "+r"(r0)                                  \
        : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)         \
        : "memory" );                                                  \
    return r0;

static inline void exit(int code) {
    _sa1(1,code);
    asm volatile( "svc #0" : : "r"(r7), "r"(r0) : "memory" );
    __builtin_unreachable();
}

static inline long read(int fd, void *buf, size_t count)
    { _syscall3(3, fd, buf, count); }

static inline long write(int fd, const void *buf, size_t count)
    { _syscall3(4, fd, buf, count); }

static inline long open(const char *pathname, int flags, int mode)
    { _syscall3(5, pathname, flags, mode); }

static inline long close(int fd)
    { _syscall1(6, fd); }

static inline long execve(const char *pathname,
                          char *const argv[], char *const envp[])
    { _syscall3(11, pathname, argv, envp); }

static inline long getpid(void)
    { _syscall0(20); }

static inline int ioctl(int fd, unsigned long request, void *arg)
    { _syscall3(54, fd, request, arg); }

static inline int dup2(int oldfd, int newfd)
    { _syscall2(63, oldfd, newfd); }

static inline pid_t setsid(void)
    { _syscall0(66); }

static inline long readlink(const char *pathname, char *buf, size_t bufsize)
    { _syscall3(85, pathname, buf, bufsize); }

static inline long munmap(void *addr, size_t length)
    { _syscall2(91, addr, length); }

static inline long statfs(const char *path, struct statfs *buf)
    { _syscall2(99, path, buf); }

static inline long sysinfo(struct sysinfo *buf)
    { _syscall1(116, buf); }

static inline long nanosleep(const struct timespec *request, struct timespec *remain)
    { _syscall2(162, request, remain); }

static inline long mremap_raw(void *old_address, size_t old_size,
                              size_t new_size, int flags)
    { _syscall4(163, old_address, old_size, new_size, flags); }

static inline void *mremap(void *old_address, size_t old_size,
                           size_t new_size, int flags)
    { return (void *)mremap_raw(old_address, old_size, new_size, flags); }

static inline long poll(struct pollfd *fds, unsigned long nfds, int timeout)
    { _syscall3(168, fds, nfds, timeout); }

static inline long rt_sigaction(int sig, const struct sigaction *act,
             struct sigaction *oact, size_t sigsetsize)
    { _syscall4(174, sig, act, oact, sigsetsize); }

static inline long getcwd(char *buf, size_t size)
    { _syscall2(183, buf, size); }

static inline long mmap_raw(void *addr, size_t length, int prot, int flags,
                            int fd, long offset)
    { _syscall6(192, addr, length, prot, flags, fd, offset); }

static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, long offset)
    { return (void *)mmap_raw(addr, length, prot, flags, fd, offset); }

static inline long clock_gettime(int clk_id, struct timespec *tp)
    { _syscall2(263, clk_id, tp); }

static inline long socket(int domain, int type, int protocol)
    { _syscall3(281, domain, type, protocol); }

static inline long connect(int sockfd, const struct sockaddr *addr, int addrlen)
    { _syscall3(283, sockfd, addr, addrlen); }

static inline long sendto(int sockfd, const void *buf, size_t len, int flags,
       const struct sockaddr *dest_addr, int addrlen)
    { _syscall6(290, sockfd, buf, len, flags, dest_addr, addrlen); }

static inline long recv(int sockfd, void *buf, size_t len, int flags)
    { _syscall4(291, sockfd, buf, len, flags); }

static inline long recvfrom(int sockfd, void *buf, size_t len, int flags,
         struct sockaddr *src_addr, int *addrlen)
    { _syscall6(292, sockfd, buf, len, flags, src_addr, addrlen); }

static inline long getrandom(void *buf, size_t buflen, unsigned int flags)
    { _syscall3(384, buf, buflen, flags); }

//////// main

#ifdef EXPORT__START
// prototype for main
__attribute__((noreturn)) static void main(int argc, char **argv);
//__attribute__((noreturn)) static int main(int argc, char **argv);

__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    register void *sp asm("sp");
    long argc = ((long *)sp)[0];
    char **argv = (char **)((long *)sp + 1);
    main(argc, argv);
    __builtin_unreachable();
}
#endif

#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    // argc/argv retrieval from stack
    void *sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    long argc = ((long *)sp)[0];
    char **argv = (char **)((long *)sp + 1);
    main(argc, argv);
    __builtin_unreachable();
}
#endif
