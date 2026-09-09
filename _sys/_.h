#include <stddef.h>
#include <stdint.h>
//can't include sys/types.h - this redefines timespec
//#include <sys/types.h>
#include "types.h"

#include "../_lib/net.h"
#include "../_lib/mem.h"
#include "../_lib/str.h"

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
