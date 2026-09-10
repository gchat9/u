///// sizes

#ifdef __SIZE_TYPE__
/* horrible kludge to make sure size_t and ssize_t are both long or both int */
#define unsigned signed
typedef __SIZE_TYPE__ ssize_t;
#undef unsigned
#else
typedef signed long ssize_t;            /* Used for a count of bytes or an error indication. */
#endif

///// clock / time

typedef int32_t clockid_t;

struct timespec {
    long tv_sec;   // seconds
    long tv_nsec;  // nanoseconds
};

/* Linux statfs(2) result layout.  long deliberately follows the target ABI. */
struct statfs {
    long f_type;
    long f_bsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    int32_t f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1

/////

typedef int32_t pid_t;        /* Used for process IDs and process group IDs. */
typedef uint32_t dev_t;       /* Used for device IDs. */
typedef uint32_t clock_t;     /* Used for system times in
                                 clock ticks or CLOCKS_PER_SEC
                                 (see <time.h>). */

#if defined(__aarch64__) || defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64)
    typedef uint32_t gid_t;   /* Used for group IDs. */
    typedef uint32_t mode_t;  /* Used for some file attributes. */
    typedef uint32_t nlink_t; /* Used for link counts. */
    typedef uint32_t uid_t;   /* Used for user IDs. */
#elif defined(__arm__) || defined(__i386__)
    typedef uint16_t gid_t;
    typedef uint16_t mode_t;
    typedef uint16_t nlink_t;
    typedef uint16_t uid_t;
#endif

///// poll

struct pollfd {
    int   fd;
    short events;
    short revents;
};

///// signal

typedef union sigval {
  int sival_int;
  void *sival_ptr;
} sigval_t;

#define SI_MAX_SIZE     128
#if __WORDSIZE == 64
#define SI_PAD_SIZE     ((SI_MAX_SIZE/sizeof(int32_t)) - 4)
#else
#define SI_PAD_SIZE     ((SI_MAX_SIZE/sizeof(int32_t)) - 3)
#endif

typedef long __band_t;

typedef struct siginfo {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  union {
    int32_t _pad[SI_PAD_SIZE];
    /* kill() */
    struct {
      pid_t _pid;               /* sender's pid */
      uid_t _uid;               /* sender's uid */
    } _kill;
    /* POSIX.1b timers */
    struct {
      uint32_t _timer1;
      uint32_t _timer2;
    } _timer;
    /* POSIX.1b signals */
    struct {
      pid_t _pid;               /* sender's pid */
      uid_t _uid;               /* sender's uid */
      sigval_t _sigval;
    } _rt;
    /* SIGCHLD */
    struct {
      pid_t _pid;               /* which child */
      uid_t _uid;               /* sender's uid */
      int32_t _status;          /* exit code */
      clock_t _utime;
      clock_t _stime;
    } _sigchld;
    /* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
    struct {
      void *_addr; /* faulting insn/memory ref. */
    } _sigfault;
    /* SIGPOLL */
    struct {
      __band_t _band;   /* POLL_IN, POLL_OUT, POLL_MSG */
      int32_t _fd;
    } _sigpoll;
  } _sifields;
} siginfo_t;

/*
 * How these fields are to be accessed.
 */
#define si_pid          _sifields._kill._pid
#define si_uid          _sifields._kill._uid
#define si_status       _sifields._sigchld._status
#define si_utime        _sifields._sigchld._utime
#define si_stime        _sifields._sigchld._stime
#define si_value        _sifields._rt._sigval
#define si_int          _sifields._rt._sigval.sival_int
#define si_ptr          _sifields._rt._sigval.sival_ptr
#define si_addr         _sifields._sigfault._addr
#define si_band         _sifields._sigpoll._band
#define si_fd           _sifields._sigpoll._fd

#define _SIGSET_WORDS   (1024 / (8 * sizeof (unsigned long int)))

typedef struct {
  unsigned long sig[_SIGSET_WORDS];
} sigset_t;

#define SIG_BLOCK       0       /* for blocking signals */
#define SIG_UNBLOCK     1       /* for unblocking signals */
#define SIG_SETMASK     2       /* for setting the signal mask */

typedef int sig_atomic_t;

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0L)      /* default signal handling */
#define SIG_IGN ((sighandler_t)1L)      /* ignore signal */
#define SIG_ERR ((sighandler_t)-1L)     /* error return from signal */

struct sigaction {
  union {
    sighandler_t _sa_handler;
    void (*_sa_sigaction)(int, siginfo_t*, void*);
  } _u;
  unsigned long sa_flags;
  void (*sa_restorer)(void);
  sigset_t sa_mask;
};
