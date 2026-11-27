// errno.h - error numbers, as Linux has them

#define EPERM            1  /* operation not permitted */
#define ENOENT           2  /* no such file or directory */
#define EINTR            4  /* interrupted system call */
#define EBADF            9  /* bad file descriptor */
#define EAGAIN          11  /* try again */
#define ENOMEM          12  /* out of memory */
#define EACCES          13  /* permission denied */
#define EIO              5  /* input/output error */
#define EBUSY           16  /* device or resource busy */
#define EEXIST          17  /* file exists */
#define EXDEV           18  /* rename across file systems */
#define ENOTDIR         20  /* not a directory */
#define EINVAL          22  /* invalid argument */
#define ENFILE          23  /* too many open files in system */
#define EMFILE          24  /* too many open files */
#define ENOSPC           28  /* no space left on device */
#define EROFS            30  /* read-only file system */
#define EPIPE            32  /* broken pipe */
#define ENAMETOOLONG    36  /* file name too long */
#define ELOOP           40  /* too many levels of symbolic links */
#define ENOTEMPTY       39  /* directory not empty */
#define EOPNOTSUPP       95  /* operation not supported */
#define ENOTCONN        107  /* transport endpoint is not connected */
#define ECANCELED      125  /* operation canceled */
#define ENETDOWN       100  /* network is down */
#define ENETUNREACH    101  /* network is unreachable */
#define ECONNRESET     104  /* connection reset by peer */
#define ETIMEDOUT      110  /* connection timed out */
#define ECONNREFUSED   111  /* connection refused */
#define EHOSTUNREACH   113  /* no route to host */
#define EINPROGRESS    115  /* operation in progress */
#define EAFNOSUPPORT    97  /* address family not supported */
#define ENOTSOCK        88  /* not a socket */
