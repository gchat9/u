// signal.h - signal numbers, and sigaction flags

#define SIGINT           2
#define SIGQUIT          3
#define SIGPIPE         13
#define SIGCHLD         17
#define SIGWINCH        28  /* terminal size changed */

#define SA_RESTORER 0x04000000UL
