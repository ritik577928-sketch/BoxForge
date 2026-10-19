/* signals.h - signal masks and waiting for the parent monitor loop */
#ifndef BF_SIGNALS_H
#define BF_SIGNALS_H

#include <signal.h>

/* Block SIGINT/SIGTERM/SIGHUP/SIGCHLD (so none is lost) and return the old mask. */
int bf_signals_block(sigset_t *old);
/* Wait up to timeout_ms; returns the signal number, 0 on timeout, -1 on error. */
int bf_signals_wait(siginfo_t *si, int timeout_ms);
/* Graceful stop of an arbitrary pid: SIGTERM, wait, then SIGKILL. 0 if gone. */
int bf_signals_stop_pid(pid_t pid, int timeout_sec);

#endif
