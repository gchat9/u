// signal.h - signal numbers, and sigaction flags

#define SIGHUP            1
#define SIGINT           2
#define SIGQUIT          3
#define SIGPIPE         13
#define SIGTERM         15
#define SIGCHLD         17
#define SIGWINCH        28  /* terminal size changed */

#define SA_SIGINFO  0x00000004UL
#define SA_RESTORER 0x04000000UL
#define SA_NODEFER  0x40000000UL
#define SA_RESTART  0x10000000UL
#define SA_RESETHAND 0x80000000UL
