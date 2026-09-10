////////////// macros

#define _sa0(n)                 register long rax asm("rax") = (long)(n);
#define _sa1(n,a)               \
        _sa0(n)                 register long rdi asm("rdi") = (long)(a);
#define _sa2(n,a,b)             \
        _sa1(n,a)               register long rsi asm("rsi") = (long)(b);
#define _sa3(n,a,b,c)           \
        _sa2(n,a,b)             register long rdx asm("rdx") = (long)(c);
#define _sa4(n,a,b,c,d)         \
        _sa3(n,a,b,c)           register long r10 asm("r10") = (long)(d);
#define _sa5(n,a,b,c,d,e)       \
        _sa4(n,a,b,c,d)         register long r8  asm("r8")  = (long)(e);
#define _sa6(n,a,b,c,d,e,f)     \
        _sa5(n,a,b,c,d,e)       register long r9  asm("r9")  = (long)(f);

#define _syscall0(n) _sa0(n)                                           \
    asm volatile( "syscall" : "+r"(rax)                                \
        :: "rcx", "r11", "memory" );                                   \
    return rax;

#define _syscall1(n,a) _sa1(n,a)                                       \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi)                                                     \
        : "rcx", "r11", "memory" );                                    \
    return rax;

#define _syscall2(n,a,b) _sa2(n,a,b)                                   \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi), "r"(rsi)                                           \
        : "rcx", "r11", "memory" );                                    \
    return rax;

#define _syscall3(n,a,b,c) _sa3(n,a,b,c)                               \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi), "r"(rsi), "r"(rdx)                                 \
        : "rcx", "r11", "memory" );                                    \
    return rax;

#define _syscall4(n,a,b,c,d) _sa4(n,a,b,c,d)                           \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)                       \
        : "rcx", "r11", "memory" );                                    \
    return rax;

#define _syscall5(n,a,b,c,d,e) _sa5(n,a,b,c,d,e)                       \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)              \
        : "rcx", "r11", "memory" );                                    \
    return rax;

#define _syscall6(n,a,b,c,d,e,f) _sa6(n,a,b,c,d,e,f)                   \
    asm volatile( "syscall" : "+r"(rax)                                \
        : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)     \
        : "rcx", "r11", "memory" );                                    \
    return rax;

////////////// syscalls

static inline void exit(int code) {
    _sa1(60, code);
    asm volatile( "syscall" :: "r"(rax), "r"(rdi) : "memory" );
    __builtin_unreachable();
}

static inline long read(int fd, void *buf, size_t count)
    { _syscall3(0, fd, buf, count); }

static inline long write(int fd, const void *buf, size_t count)
    { _syscall3(1, fd, buf, count); }

static inline long open(const char *pathname, int flags, int mode)
    { _syscall3(2, pathname, flags, mode); }

static inline long close(int fd)
    { _syscall1(3, fd); }

static inline long poll(struct pollfd *fds, unsigned long nfds, int timeout)
    { _syscall3(7, fds, nfds, timeout); }

static inline long mmap_raw(void *addr, size_t length, int prot, int flags,
                            int fd, long offset)
    { _syscall6(9, addr, length, prot, flags, fd, offset); }

static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, long offset)
    { return (void *)mmap_raw(addr, length, prot, flags, fd, offset); }

static inline long munmap(void *addr, size_t length)
    { _syscall2(11, addr, length); }

static inline long rt_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oact, size_t sigsetsize)
    { _syscall4(13, sig, act, oact, sigsetsize); }

static inline int ioctl(int fd, unsigned long request, void *arg)
    { _syscall3(16, fd, request, arg); }

static inline long mremap_raw(void *old_address, size_t old_size,
                              size_t new_size, int flags)
    { _syscall4(25, old_address, old_size, new_size, flags); }

static inline void *mremap(void *old_address, size_t old_size,
                           size_t new_size, int flags)
    { return (void *)mremap_raw(old_address, old_size, new_size, flags); }

static inline long dup2(int oldfd, int newfd)
    { _syscall2(33, oldfd, newfd); }

static inline pid_t setsid(void)
    { _syscall0(112); }

static inline long nanosleep(const struct timespec *request,
                             struct timespec *remain)
    { _syscall2(35, request, remain); }

static inline long getpid(void)
    { _syscall0(39); }

static inline long socket(int domain, int type, int protocol)
    { _syscall3(41, domain, type, protocol); }

static inline long connect(int sockfd,
                           const struct sockaddr *addr, int addrlen)
    { _syscall3(42, sockfd, addr, addrlen); }

static inline long sendto(int sockfd,
                          const void *buf, size_t len, int flags,
                          const struct sockaddr *dest_addr, int addrlen)
    { _syscall6(44, sockfd, buf, len, flags, dest_addr, addrlen); }

static inline long recvfrom(int sockfd, void *buf, size_t len, int flags,
                            struct sockaddr *src_addr, int *addrlen)
    { _syscall6(45, sockfd, buf, len, flags, src_addr, addrlen); }

/* recv does not exist as separate syscall here; express via recvfrom */
#define recv(sockfd, buf, len, flags) recvfrom(sockfd, buf, len, flags, 0, 0)

static inline long execve(const char *pathname,
                          char *const argv[], char *const envp[])
    { _syscall3(59, pathname, argv, envp); }

static inline long getcwd(char *buf, size_t size)
    { _syscall2(79, buf, size); }

static inline long statfs(const char *path, struct statfs *buf)
    { _syscall2(137, path, buf); }

static inline long clock_gettime(clockid_t clk_id, struct timespec *ts)
    { _syscall2(228, clk_id, ts); }

static inline long getrandom(void *buf, size_t buflen, unsigned int flags)
    { _syscall3(318, buf, buflen, flags); }

#ifdef EXPORT__START
// prototype for main
__attribute__((noreturn)) static void main(int argc, char **argv);

__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    // argc/argv retrieval from stack
    long *rsp;
    asm volatile("movq %%rsp, %0" : "=r"(rsp));
    long argc = rsp[0];
    char ** argv = (char **)(rsp + 1);
    main(argc, argv);
}
#endif
