/* signals.c */
#include "signals.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "boxforge.h"

static void monitored_set(sigset_t *s)
{
    sigemptyset(s);
    sigaddset(s, SIGINT);
    sigaddset(s, SIGTERM);
    sigaddset(s, SIGHUP);
    sigaddset(s, SIGCHLD);
}

int bf_signals_block(sigset_t *old)
{
    sigset_t s;

    monitored_set(&s);
    return sigprocmask(SIG_BLOCK, &s, old);
}

int bf_signals_wait(siginfo_t *si, int timeout_ms)
{
    sigset_t s;
    struct timespec ts = {.tv_sec = timeout_ms / 1000,
                          .tv_nsec = (long)(timeout_ms % 1000) * 1000000L};
    int r;

    monitored_set(&s);
    r = sigtimedwait(&s, si, &ts);
    if (r < 0)
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return r;
}

int bf_signals_stop_pid(pid_t pid, int timeout_sec)
{
    int i;

    if (!bf_pid_alive(pid))
        return 0;
    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < timeout_sec * 10; i++) {
        if (!bf_pid_alive(pid))
            return 0;
        bf_msleep(100);
    }
    bf_warn("pid %d ignored SIGTERM for %ds, sending SIGKILL", (int)pid, timeout_sec);
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < 50; i++) {
        if (!bf_pid_alive(pid))
            return 0;
        bf_msleep(100);
    }
    return -1;
}
