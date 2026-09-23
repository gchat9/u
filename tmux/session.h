#pragma once

#include "window.h"
#include "config.h"

/*
 * Create, bind, and listen on MUX_SOCKET_PATH.
 * Call at startup (before entering raw mode) so that bind errors surface
 * cleanly and so that the socket exists for the lifetime of the session,
 * enabling reattach after SSH disconnect.
 *
 * Returns the listening fd on success, -1 on error (message printed).
 */
int session_listen(void);

/* Like session_listen but skips the liveness probe (use after daemon detach). */
int session_rebind(void);

/*
 * Detach: fork a headless daemon that holds the session state and PTY
 * master fds, then exit the attached process.  sock is the already-bound
 * listening fd from session_listen(); it is inherited by the daemon.
 *
 * The caller must restore the terminal (leave alt screen, input_restore)
 * before calling this.  This function never returns.
 */
void session_detach(int sock, Window *wins[], int cur);

#include <stdint.h>

/*
 * Observer frame header.  Sent by the live instance before each frame.
 * rows=-1 is a takeover sentinel: observer should exit and re-attach.
 * rows=0  signals EOF / disconnect.
 */
typedef struct __attribute__((packed)) {
    int16_t  rows, cols;
    int16_t  cur_row, cur_col;
    uint8_t  cur_visible;
    uint8_t  cur_win;
    uint16_t win_exists_mask;
    uint16_t win_alive_mask;
    int32_t  win_pids[10];
    uint16_t status_len;
} ObsHeader;

/* Connection type bytes */
#define SESSION_TYPE_ATTACH  'A'
#define SESSION_TYPE_OBSERVE 'O'
#define SESSION_TYPE_TAKEOVER 'T'

/*
 * Server-side takeover: send session to new main, recv its size.
 * Returns 0 on success.
 */
int session_do_takeover(int conn, int session_sock,
                        Window *wins[], int cur,
                        int *obs_fd_out, int *rows_out, int *cols_out);

/* Internal: shared observe loop entered after connection and size exchange. */
int session_observe_fd(int sock, int ses_rows, int ses_cols);

/*
 * Called by the old-live instance after handing session to a takeover client.
 * conn is the still-open takeover connection; the new main will push frames
 * back over it.  rows/cols are the new main's terminal size.
 * Restores the terminal and enters the observe loop.
 */
int  session_become_observer(int conn, int rows, int cols);

/*
 * Observe: connect to a live mux instance, receive a stream of screen
 * frames, and render them on the local terminal (read-only).
 *
 * Runs until the remote instance exits or the connection drops.
 * Returns 0 on clean disconnect, -1 on error.
 */
int session_observe(void);

/*
 * Server side: called by the live mux instance when an observer connects.
 * obs_fd is the accepted connection fd (already past the type byte).
 * Sends the initial screen frame for s; returns 0 on success.
 */
int session_observer_init(int obs_fd, const Screen *s, int rows, int cols,
                          Window *wins[], int cur_win);

/*
 * Push one screen frame (with status bar) to an observer.
 * Returns 0 on success, -1 if the observer disconnected.
 */
int session_observer_push(int obs_fd, const Screen *s,
                          Window *wins[], int cur_win);

/*
 * Attach: connect to the daemon at MUX_SOCKET_PATH, receive all window
 * state and PTY master fds, and populate wins[].
 *
 * On success: wins[] is populated, *cur_out is the active window index.
 * The caller is responsible for calling vt_resize()+pty_resize() if the
 * new terminal size differs from the daemon's.
 *
 * Returns 0 on success, -1 on error (message printed to stderr).
 */
int session_attach(Window *wins[], int *cur_out,
                   int *new_session_sock_out, int *obs_fd_out);
