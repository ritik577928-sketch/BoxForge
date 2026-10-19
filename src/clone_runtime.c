/* clone_runtime.c
 *
 * Child side of a container.  Sequence (after the parent says GO):
 *   userns identity -> hostname -> loopback/veth -> private mounts ->
 *   overlay -> bind rootfs, /dev, /proc -> pivot_root -> drop caps ->
 *   env -> exec  (direct mode)   or   fork+supervise (--init mode)
 *
 * PID-1 design
 * ------------
 * The cloned child is PID 1 of the new PID namespace.
 *  - direct mode (default): the child exec()s the target, so the *target* is
 *    PID 1.  It is then responsible for reaping orphans, which an arbitrary
 *    program does not do.  Good for demos (`echo $$` prints 1).
 *  - --init mode: the child stays as a tiny init/supervisor, forks the target
 *    (PID 2), reaps every orphan, forwards SIGTERM/SIGINT/SIGHUP/SIGUSR1/2
 *    and exits with the target's status.  Robust, but the target is not PID 1.
 */
#include "clone_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "namespaces.h"
#include "network.h"
#include "overlay.h"
#include "rootfs.h"
#include "userns.h"

/* Report a setup error to the parent through the error pipe and exit. */
static void child_fail(const struct container *c, int code, const char *what)
{
    char msg[512];
    int saved = errno;
    int n = snprintf(msg, sizeof msg, "%d %s: %s\n", code, what, strerror(saved));

    if (n > 0)
        (void)bf_write_all(c->err_pipe[1], msg, (size_t)(n < (int)sizeof msg ? n : (int)sizeof msg - 1));
    _exit(code);
}

#define TRY(expr, what)                                                                          \
    do {                                                                                         \
        if ((expr) < 0)                                                                          \
            child_fail(c, BF_EXIT_SETUP, (what));                                                \
    } while (0)

static void exec_app(const struct container *c)
{
    signal(SIGPIPE, SIG_DFL);
    execvp(c->argv[0], c->argv);
    child_fail(c, errno == ENOENT ? BF_EXIT_NOTFOUND : BF_EXIT_NOEXEC, c->argv[0]);
}

static void noop_handler(int sig) { (void)sig; }

static int status_to_code(int st)
{
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* Reap anything left in the namespace, bounded by timeout_ms. */
static void reap_all(int timeout_ms)
{
    int waited = 0;

    for (;;) {
        pid_t p = waitpid(-1, NULL, WNOHANG);
        if (p > 0)
            continue;
        if (p < 0 && errno != EINTR)
            return; /* ECHILD: nothing left */
        if (waited >= timeout_ms)
            return;
        bf_msleep(25);
        waited += 25;
    }
}

static int run_init(const struct container *c)
{
    static const int fwd[] = {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGQUIT};
    sigset_t set, old;
    struct sigaction sa;
    pid_t app;
    int code = 0;
    size_t i;
    bool done = false;

    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    for (i = 0; i < sizeof fwd / sizeof fwd[0]; i++)
        sigaddset(&set, fwd[i]);
    sigprocmask(SIG_BLOCK, &set, &old);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof fwd / sizeof fwd[0]; i++)
        sigaction(fwd[i], &sa, NULL); /* PID 1 only receives signals that have a handler */

    app = fork();
    if (app < 0)
        child_fail(c, BF_EXIT_SETUP, "fork (init mode)");
    if (app == 0) {
        for (i = 0; i < sizeof fwd / sizeof fwd[0]; i++)
            signal(fwd[i], SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        sigprocmask(SIG_SETMASK, &old, NULL);
        exec_app(c);
    }
    close(c->err_pipe[1]); /* only the app keeps it open (until exec) */

    while (!done) {
        siginfo_t si;
        int sig = sigwaitinfo(&set, &si);

        if (sig < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (sig == SIGCHLD) {
            int st;
            pid_t p;
            while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
                if (p == app) {
                    code = status_to_code(st);
                    done = true;
                }
            }
        } else {
            kill(app, sig);
        }
    }
    /* Orderly teardown: ask leftovers to stop, reap them, then exit.  When
     * this process exits the kernel SIGKILLs anything still in the namespace. */
    kill(-1, SIGTERM);
    reap_all(2000);
    kill(-1, SIGKILL);
    reap_all(500);
    _exit(code);
}

static int child_main(void *arg)
{
    struct container *c = arg;
    const char *root;
    char go = 0;
    ssize_t n;

    close(c->sync_pipe[1]);
    close(c->err_pipe[0]);
    if (c->ready_fd >= 0)
        close(c->ready_fd);
    sigprocmask(SIG_SETMASK, &c->saved_mask, NULL);
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    /* Wait for the parent: cgroup membership (and uid/gid maps, veth) must be
     * in place before anything of the application can run. */
    do {
        n = read(c->sync_pipe[0], &go, 1);
    } while (n < 0 && errno == EINTR);
    close(c->sync_pipe[0]);
    if (n != 1 || go != 'G')
        _exit(BF_EXIT_SETUP);

    if (c->opt_userns)
        TRY(bf_userns_become_root(), "become uid/gid 0 in user namespace");
    TRY(bf_ns_set_hostname(c->hostname), "sethostname");
    TRY(bf_ns_loopback_up(), "bring up loopback");
    if (c->opt_net_bridge)
        TRY(bf_net_child_configure(c), "configure eth0");

    /* Mount namespace: private propagation FIRST so nothing leaks to the host. */
    TRY(bf_rootfs_make_private(), "make mounts private");
    root = c->rootfs;
    if (c->opt_overlay) {
        TRY(bf_overlay_mount(c), "mount overlay");
        root = c->overlay_merged;
    }
    TRY(bf_rootfs_prepare(root), "prepare rootfs (/dev, /proc)");
    if (c->opt_chroot)
        TRY(bf_rootfs_chroot(root), "chroot");
    else
        TRY(bf_rootfs_pivot(root), "pivot_root");
    TRY(chdir("/"), "chdir /");

    if (!c->opt_keep_caps)
        TRY(bf_ns_drop_capabilities(), "drop capabilities");
    bf_ns_setup_env(c);

    if (c->opt_init)
        run_init(c);
    exec_app(c);
    return BF_EXIT_SETUP; /* not reached */
}

int bf_clone_container(struct container *c)
{
    char *stack, *top;
    pid_t pid;
    int saved;

    /* glibc clone() wants the *top* of a stack that grows down.  A guard page
     * at the bottom turns an overflow into a clean SIGSEGV. */
    stack = mmap(NULL, BF_STACK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        bf_err("mmap child stack: %s", strerror(errno));
        return -1;
    }
    if (mprotect(stack, (size_t)sysconf(_SC_PAGESIZE), PROT_NONE) < 0)
        bf_warn("mprotect guard page: %s", strerror(errno));
    top = stack + BF_STACK_SIZE;

    pid = clone(child_main, top, bf_ns_clone_flags(c), c);
    saved = errno;
    /* No CLONE_VM: the child owns a private copy, so the parent can free it. */
    munmap(stack, BF_STACK_SIZE);
    if (pid < 0) {
        errno = saved;
        bf_err("clone: %s (need root / CAP_SYS_ADMIN; user namespaces may be disabled)",
               strerror(saved));
        return -1;
    }
    c->host_pid = pid;
    return 0;
}
