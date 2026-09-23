#pragma once
#include <sys/types.h>

typedef struct {
    int   master;   /* fd to read child output / write input  */
    pid_t child;    /* pid of the child shell                 */
} Pty;

/*
 * Open a PTY pair, fork, and exec `shell` (or $SHELL / /bin/bash).
 * The child's stdin/stdout/stderr are all on the slave side.
 * `rows` and `cols` are set on the PTY before exec.
 * Returns 0 on success, -1 on error (errno set).
 */
/*
 * If argv is non-NULL, exec argv[0] with the given argument vector.
 * If argv is NULL, exec DEFAULT_SHELL (or $SHELL / /bin/bash).
 */
int pty_open(Pty *pty, int rows, int cols, char *const *argv);

/* Close the master fd (does not kill the child). */
void pty_close(Pty *pty);

/* Resize the kernel PTY window-size struct. */
int pty_resize(Pty *pty, int rows, int cols);
