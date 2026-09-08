#define _sa0(n)                 register long rax asm("rax") = (long)(n);
#define _sa1(n,a)               \
    _sa0(n)                     register long rdi asm("rdi") = (long)(a);
#define _sa2(n,a,b)             \
    _sa1(n,a)                   register long rsi asm("rsi") = (long)(b);
#define _sa3(n,a,b,c)           \
    _sa2(n,a,b)                 register long rdx asm("rdx") = (long)(c);

#define _syscall0(n) _sa0(n)                                           \
    asm volatile( "syscall" : "+r"(rax)                                \
        :: "rcx", "r11", "memory" );                                   \
    return rax;

#define _syscall1(n,a,b) _sa1(n,a)                                     \
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

#define __NR_read             0
#define __NR_write            1
#define __NR_open             2
#define __NR_close            3
#define __NR_nanosleep       35
#define __NR_getcwd          79
#define __NR_getpid          39
#define __NR_exit            60
#define __NR_clock_gettime  228
#define __NR_getrandom      318

typedef long         ptr_t;

static inline long read(int fd, void *buf, long count) {
    register long rax asm("rax") = __NR_read;
    register long rdi asm("rdi") = (long)fd;
    register long rsi asm("rsi") = (long)buf;
    register long rdx asm("rdx") = count;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long write(int fd, const void *buf, long count) {
    register long rax asm("rax") = __NR_write;
    register long rdi asm("rdi") = (long)fd;
    register long rsi asm("rsi") = (long)buf;
    register long rdx asm("rdx") = count;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long open(const char *pathname, long flags, long mode) {
    register long rax asm("rax") = __NR_open;
    register long rdi asm("rdi") = (long)pathname;
    register long rsi asm("rsi") = flags;
    register long rdx asm("rdx") = mode;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long close(int fd) {
    register long rax asm("rax") = __NR_close;
    register long rdi asm("rdi") = (long)fd;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long execve(const char *pathname,
                          char *const argv[], char *const envp[])
    { _syscall3(59, pathname, argv, envp); }

static inline void exit(int code)
{
    register long rax asm("rax") = __NR_exit;
    register long rdi asm("rdi") = code;

    asm volatile(
        "syscall"
        :
        : "r"(rax), "r"(rdi)
        : "memory"
    );
    __builtin_unreachable();
}

pid_t setsid(void)
    { _syscall0(112); }

/*
struct timespec {
    long tv_sec;   // seconds
    long tv_nsec;  // nanoseconds
}; */

static inline long clock_gettime(clockid_t clk_id, struct timespec *ts) {
    register long rax asm("rax") = __NR_clock_gettime;
    register long rdi asm("rdi") = (long)clk_id;
    register long rsi asm("rsi") = (long)ts;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline int ioctl(int fd, unsigned long request, void *arg)
    { _syscall3(16, fd, request, arg); }

static inline int dup2(int oldfd, int newfd)
    { _syscall2(33, oldfd, newfd); }

static inline long nanosleep(const struct timespec *request,
                                   struct timespec *remain) {
    register long rax asm("rax") = __NR_nanosleep;
    register long rdi asm("rdi") = (long)request;
    register long rsi asm("rsi") = (long)remain;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long getpid(void) {
    register long rax asm("rax") = __NR_getpid;

    asm volatile(
        "syscall"
        : "+r"(rax)
        :
        : "rcx", "r11"
    );
    return rax;
}

static inline long getrandom(void *buf,
                             size_t buflen,
                             unsigned int flags) {
    register long rax asm("rax") = __NR_getrandom;
    register long rdi asm("rdi") = (long)buf;
    register long rsi asm("rsi") = (long)buflen;
    register long rdx asm("rdx") = (long)flags;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );

    return rax;
}

static inline long getcwd(char *buf, size_t size) {
    register long rax asm("rax") = __NR_getcwd;
    register long rdi asm("rdi") = (long)buf;
    register long rsi asm("rsi") = (long)size;

    asm volatile(
        "syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi)
        : "rcx", "r11", "memory"
    );

    return rax;
}

#ifdef EXPORT__START
// prototype for main
__attribute__((noreturn)) static void main(int argc, char **argv);

// following are alternative implementations of _start
// first
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    // argc/argv retrieval from stack
    long *rsp;
    asm volatile("movq %%rsp, %0" : "=r"(rsp));
    long argc = rsp[0];
    char ** argv = (char **)(rsp + 1);
    main(argc, argv);
}
#endif

//second
#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    long argc;
    char **argv;

    __asm__ volatile (
        "movq (%%rsp), %0\n\t"
        "lea 8(%%rsp), %1\n\t"
        : "=r" (argc), "=r" (argv)
        :
        : "memory"
    );
    main(argc, argv);
}
#endif

//third
#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    long argc;
    char **argv;

    asm volatile (
        "popq %0\n\t"        // Load argc, RSP += 8
        "movq %%rsp, %1\n\t" // Load argv
        "pushq %0\n\t"       // Restore/re-align stack
        : "=r" (argc), "=r" (argv)
        :
        : "memory"
    );

    main(argc, argv);
}
#endif

//fourth, not compatible with inlining
#if 0
__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    long argc;
    char **argv;
    // 1. pop argc (1 byte). RSP is now 8-byte aligned (ending in 8).
    // 2. mov argv, rsp (3 bytes).
    // 3. jmp main (5 bytes, or 2 if short jump).
    __asm__ volatile (
        "popq %0\n\t"
        "movq %%rsp, %1\n\t"
        "jmp main\n\t"
        : "=r" (argc), "=r" (argv) : : "memory"
    );
    __builtin_unreachable();
}
#endif
