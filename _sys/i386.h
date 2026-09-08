////////////// macros

#define _sa0(n)             register long eax asm("eax") = (long)(n);
#define _sa1(n,a)           \
        _sa0(n)             register long ebx asm("ebx") = (long)(a);
#define _sa2(n,a,b)         \
        _sa1(n,a)           register long ecx asm("ecx") = (long)(b);
#define _sa3(n,a,b,c)       \
        _sa2(n,a,b)         register long edx asm("edx") = (long)(c);
#define _sa4(n,a,b,c,d)     \
        _sa3(n,a,b,c)       register long esi asm("esi") = (long)(d);
#define _sa5(n,a,b,c,d,e)   \
        _sa4(n,a,b,c,d)     register long edi asm("edi") = (long)(e);
#define _sa6(n,a,b,c,d,e,f) \
        _sa5(n,a,b,c,d,e)   register long ebp asm("ebp") = (long)(f);

#define _syscall0(n) _sa0(n)                             \
    asm volatile( "int $0x80" : "+r"(eax) :: "memory" ); \
    return eax;

#define _syscall1(n,a) _sa1(n,a)            \
    asm volatile( "int $0x80" : "+r"(eax)   \
        : "r"(ebx)                          \
        : "memory" );                       \
    return eax;

#define _syscall2(n,a,b) _sa2(n,a,b)        \
    asm volatile( "int $0x80" : "+r"(eax)   \
        : "r"(ebx), "r"(ecx)                \
        : "memory" );                       \
    return eax;

#define _syscall3(n,a,b,c) _sa3(n,a,b,c)      \
    asm volatile( "int $0x80" : "+r"(eax)     \
        : "r"(ebx), "r"(ecx), "r"(edx)        \
        : "memory" );                         \
    return eax;

#define _syscall4(n,a,b,c,d) _sa4(n,a,b,c,d)       \
    asm volatile( "int $0x80" : "+r"(eax)          \
        : "r"(ebx), "r"(ecx), "r"(edx), "r"(esi)   \
        : "memory" );                              \
    return eax;

#define _syscall5(n,a,b,c,d,e) _sa5(n,a,b,c,d,e)            \
    asm volatile( "int $0x80" : "+r"(eax)                   \
        : "r"(ebx), "r"(ecx), "r"(edx), "r"(esi), "r"(edi)  \
        : "memory" );                                       \
    return eax;

#define _syscall6(n,a,b,c,d,e,f) _sa6(n,a,b,c,d,e,f)                 \
    asm volatile( "int $0x80" : "+r"(eax)                            \
        : "r"(ebx), "r"(ecx), "r"(edx), "r"(esi), "r"(edi), "r"(ebp) \
        : "memory" );                                                \
    return eax;

////////////// syscalls

static inline __attribute__((noreturn)) void exit(int code) {
    _sa1(1, code);
    asm volatile( "int $0x80" :: "r"(eax), "r"(ebx) : "memory" );
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

static inline long execve(const char *pathname, char *const argv[], char *const envp[])
    { _syscall3(11, pathname, argv, envp); }

static inline long getpid(void)
    { _syscall0(20); }

static inline int ioctl(int fd, unsigned long request, void *arg)
    { _syscall3(54, fd, request, arg); }

static inline int dup2(int oldfd, int newfd)
    { _syscall2(63, oldfd, newfd); }

static inline pid_t setsid(void)
    { _syscall0(66); }

static inline long nanosleep(const struct timespec *request,
                                       struct timespec *remain)
    { _syscall2(162, request, remain); }

static inline long poll(struct pollfd *fds, unsigned long nfds,
                                  int timeout)
    { _syscall3(168, fds, nfds, timeout); }

static inline long rt_sigaction(int sig, const struct sigaction *act,
                                          struct sigaction *oact, size_t sigsetsize)
    { _syscall4(174, sig, act, oact, sigsetsize); }

static inline long clock_gettime(int clk_id, struct timespec *tp)
    { _syscall2(265, clk_id, tp); }

static inline long getrandom(void *buf, size_t buflen, unsigned int flags)
    { _syscall3(355, buf, buflen, flags); }

static inline long getcwd(char *buf, size_t size)
    { _syscall2(183, buf, size); }

static inline long socket(int domain, int type, int protocol)
    { _syscall3(359, domain, type, protocol); }

static inline long connect(int sockfd,
                           const struct sockaddr *addr, int addrlen)
    { _syscall3(362, sockfd, addr, addrlen); }

static inline long sendto(int sockfd,
                          const void *buf, size_t len, int flags,
                          const struct sockaddr *dest_addr, int addrlen)
    { _syscall6(369, sockfd, buf, len, flags, dest_addr, addrlen); }

static inline long recvfrom(int sockfd, void *buf, size_t len, int flags,
                            struct sockaddr *src_addr, int *addrlen)
    { _syscall6(371, sockfd, buf, len, flags, src_addr, addrlen); }

/* No dedicated recv syscall on x86-32; recvfrom with NULL addr/addrlen. */
#define recv(sockfd, buf, len, flags) recvfrom(sockfd, buf, len, flags, 0, 0)

////////////// main

#ifdef EXPORT__START
// prototype for main
__attribute__((noreturn)) static void main(int argc, char **argv);

__attribute__((naked))
__attribute__((noreturn)) void _start(void) {
    long *esp;
    asm volatile(
        // gcc-emitted code assume ebp contains "original esp", so copy that
        "mov  %%esp,%%ebp\n"
        // argc/argv retrieval from stack
        "movl %%esp, %0" : "=r"(esp));
    long argc = esp[0];
    char **argv = (char **)(esp + 1);
    main(argc, argv);
}
#endif
