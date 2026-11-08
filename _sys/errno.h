// errno.h - error numbers, as Linux has them

#define ENOENT           2  /* no such file or directory */
#define EINTR            4  /* interrupted system call */
#define EIO              5  /* input/output error */
#define EBUSY           16  /* device or resource busy */
#define EEXIST          17  /* file exists */
#define EXDEV           18  /* rename across file systems */
#define ENOTDIR         20  /* not a directory */
#define EINVAL          22  /* invalid argument */
#define EMFILE           24  /* too many open files */
#define ENOSPC           28  /* no space left on device */
#define EROFS            30  /* read-only file system */
#define ENAMETOOLONG    36  /* file name too long */
#define ENOTEMPTY       39  /* directory not empty */
#define EOPNOTSUPP       95  /* operation not supported */
#define ENOTCONN        107  /* transport endpoint is not connected */
#define ECANCELED      125  /* operation canceled */
