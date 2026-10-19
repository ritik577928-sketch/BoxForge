/* lifecycle.c - parent-side orchestration of one container */
#include "lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cgroup.h"
#include "clone_runtime.h"
#include "namespaces.h"
#include "network.h"
#include "overlay.h"
#include "signals.h"
#include "userns.h"

static void transition(struct container *c, bf_state_t st)
{
    bf_debug("container %s: %s -> %s", c->id, bf_state_name(c->state), bf_state_name(st));
    c->state = st;
    if (c->cleanup.runtime_dir)
        (void)bf_container_save(c);
}

/* ---- detach support ---------------------------------------------------- */

static void report_ready(struct container *c, bool ok)
{
    if (c->ready_fd < 0)
        return;
    if (ok)
        dprintf(c->ready_fd, "OK %s\n", c->id);
    else
        dprintf(c->ready_fd, "ERR %s\n", bf_last_error());
    close(c->ready_fd);
    c->ready_fd = -1;
}

/* Returns -1 on error, 0 in the daemon (continue), 1 in the launcher
 * (*launcher_rc holds its exit status). */
static int daemonize(struct container *c, int *launcher_rc)
{
    int rp[2];
    pid_t p;

    if (pipe2(rp, O_CLOEXEC) < 0) {
        bf_err("pipe: %s", strerror(errno));
        return -1;
    }
    fflush(NULL);
    p = fork();
    if (p < 0) {
        bf_err("fork: %s", strerror(errno));
        return -1;
    }
    if (p > 0) {
        char buf[600] = "";
        ssize_t n, total = 0;

        close(rp[1]);
        while ((n = read(rp[0], buf + total, sizeof buf - 1 - (size_t)total)) > 0 &&
               total < (ssize_t)sizeof buf - 1)
            total += n;
        close(rp[0]);
        buf[total] = '\0';
        if (strncmp(buf, "OK ", 3) == 0) {
            char *nl = strchr(buf, '\n');
            if (nl)
                *nl = '\0';
            printf("%s\n", buf + 3);
            *launcher_rc = 0;
        } else {
            fprintf(stderr, "boxforge: failed to start container: %s",
                    strncmp(buf, "ERR ", 4) == 0 ? buf + 4 : "daemon exited unexpectedly\n");
            *launcher_rc = BF_EXIT_SETUP;
        }
        return 1;
    }
    close(rp[0]);
    c->ready_fd = rp[1];
    if (setsid() < 0)
        bf_warn("setsid: %s", strerror(errno));
    {
        int in = open("/dev/null", O_RDONLY);
        int out = open(c->log_file[0] ? c->log_file : "/dev/null",
                       O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (in >= 0)
            dup2(in, STDIN_FILENO);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        if (in > 2)
            close(in);
        if (out > 2)
            close(out);
    }
    return 0;
}

/* ---- starting the container -------------------------------------------- */

static void abort_child(struct container *c)
{
    int st;

    if (c->host_pid <= 0)
        return;
    if (c->sync_pipe[1] >= 0)
        (void)bf_write_all(c->sync_pipe[1], "X", 1);
    kill(c->host_pid, SIGKILL);
    while (waitpid(c->host_pid, &st, 0) < 0 && errno == EINTR)
        ;
    c->host_pid = 0;
}

/* Read "<code> message" from the child's error pipe until EOF (= exec ok). */
static int read_child_error(struct container *c, int *code, char *msg, size_t n)
{
    char buf[400];
    size_t total = 0;
    ssize_t r;

    while (total < sizeof buf - 1) {
        r = read(c->err_pipe[0], buf + total, sizeof buf - 1 - total);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (total == 0)
        return 0;
    *code = BF_EXIT_SETUP;
    if (sscanf(buf, "%d", code) == 1) {
        char *sp = strchr(buf, ' ');
        snprintf(msg, n, "%s", sp ? sp + 1 : buf);
    } else {
        snprintf(msg, n, "%s", buf);
    }
    msg[strcspn(msg, "\n")] = '\0';
    return 1;
}

static int start_container(struct container *c, int *fail_code)
{
    char msg[512];
    int code;

    *fail_code = BF_EXIT_SETUP;
    if (!c->opt_no_cgroup && bf_cgroup_create(c) < 0)
        return -1;
    if (c->opt_overlay && bf_overlay_prepare(c) < 0)
        return -1;
    if (c->opt_net_bridge && bf_net_host_prepare(c) < 0)
        return -1;
    if (pipe2(c->sync_pipe, O_CLOEXEC) < 0 || pipe2(c->err_pipe, O_CLOEXEC) < 0) {
        bf_err("pipe2: %s", strerror(errno));
        return -1;
    }
    c->cleanup.pipes = true;

    if (bf_clone_container(c) < 0)
        return -1;
    transition(c, BF_ST_SETUP);
    close(c->sync_pipe[0]);
    c->sync_pipe[0] = -1;
    close(c->err_pipe[1]);
    c->err_pipe[1] = -1;

    /* Everything below happens while the child is blocked on the sync pipe. */
    if (c->opt_userns && bf_userns_write_maps(c, c->host_pid) < 0)
        return -1;
    if (bf_cgroup_add_pid(c, c->host_pid) < 0)
        return -1;
    if (c->opt_net_bridge && bf_net_host_attach(c) < 0)
        return -1;

    if (bf_write_all(c->sync_pipe[1], "G", 1) < 0) {
        bf_err("cannot signal child: %s", strerror(errno));
        return -1;
    }
    close(c->sync_pipe[1]);
    c->sync_pipe[1] = -1;

    if (read_child_error(c, &code, msg, sizeof msg)) {
        int st;
        bf_err("container setup failed: %s", msg);
        kill(c->host_pid, SIGKILL);
        while (waitpid(c->host_pid, &st, 0) < 0 && errno == EINTR)
            ;
        c->host_pid = 0;
        *fail_code = code;
        return -1;
    }
    close(c->err_pipe[0]);
    c->err_pipe[0] = -1;
    return 0;
}

/* ---- monitoring --------------------------------------------------------- */

static long elapsed_ms(const struct timespec *since)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L + (now.tv_nsec - since->tv_nsec) / 1000000L;
}

static int monitor(struct container *c)
{
    bool stopping = false, killed = false;
    struct timespec t_stop = {0, 0};
    int status = 0;
    int sigint_count = 0;

    for (;;) {
        siginfo_t si;
        int sig = bf_signals_wait(&si, 200);
        pid_t r;

        if (sig > 0 && sig != SIGCHLD && !stopping) {
            bool from_tty = (sig == SIGINT && (si.si_code == SI_KERNEL || si.si_code == SI_USER));
            if (sig == SIGINT && from_tty && ++sigint_count == 1) {
                bf_debug("first SIGINT: giving container process group opportunity to handle it");
            } else {
                bf_info("received signal %d: stopping container %s (SIGTERM, then SIGKILL after %ds)",
                        sig, c->id, c->stop_timeout);
                stopping = true;
                clock_gettime(CLOCK_MONOTONIC, &t_stop);
                transition(c, BF_ST_STOPPING);
                kill(c->host_pid, SIGTERM);
            }
        }
        r = waitpid(c->host_pid, &status, WNOHANG);
        if (r == c->host_pid)
            break;
        if (r < 0 && errno != EINTR) {
            bf_err("waitpid: %s", strerror(errno));
            status = 0;
            break;
        }
        if (stopping && !killed && elapsed_ms(&t_stop) >= (long)c->stop_timeout * 1000L) {
            bf_warn("container %s did not stop within %ds: sending SIGKILL", c->id,
                    c->stop_timeout);
            kill(c->host_pid, SIGKILL);
            if (c->cgroup_path[0]) {
                char p[PATH_MAX], buf[512];
                if (bf_snprintf(p, sizeof p, "%s/cgroup.kill", c->cgroup_path) == 0)
                    (void)bf_write_file(p, "1");
                if (bf_snprintf(p, sizeof p, "%s/cgroup.procs", c->cgroup_path) == 0 &&
                    bf_read_file(p, buf, sizeof buf) > 0) {
                    char *line, *save = NULL;
                    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
                        pid_t lp = (pid_t)atoi(line);
                        if (lp > 0)
                            (void)kill(lp, SIGKILL);
                    }
                }
            }
            killed = true;
        }
    }
    c->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 0);
    return c->exit_code;
}

int bf_lifecycle_run(struct container *c)
{
    int rc, fail_code = BF_EXIT_SETUP;
    unsigned long oom;

    if (c->opt_detach) {
        int lrc = 0, d = daemonize(c, &lrc);
        if (d < 0)
            return 1;
        if (d > 0)
            return lrc;
    }
    c->monitor_pid = getpid();
    if (bf_container_make_runtime_dir(c) < 0)
        goto fail;
    if (bf_signals_block(&c->saved_mask) < 0) {
        bf_err("sigprocmask: %s", strerror(errno));
        goto fail;
    }
    (void)bf_container_save(c);

    if (start_container(c, &fail_code) < 0)
        goto fail;

    transition(c, BF_ST_RUNNING);
    bf_info("container %s running (host pid %d)", c->id, (int)c->host_pid);
    report_ready(c, true);

    rc = monitor(c);

    oom = bf_cgroup_oom_kills(c);
    if (oom > 0) {
        c->oom_killed = true;
        fprintf(stderr, "boxforge: container %s was killed by the kernel OOM killer "
                        "(memory.events oom_kill=%lu, memory.max=%llu)\n",
                c->id, oom, c->mem_limit);
    }
    transition(c, BF_ST_EXITED);
    bf_container_cleanup(c);
    return rc;

fail:
    abort_child(c);
    if (c->cleanup.runtime_dir)
        transition(c, BF_ST_FAILED);
    report_ready(c, false);
    bf_container_cleanup(c);
    return fail_code;
}

/* ---- stop ---------------------------------------------------------------- */

int bf_lifecycle_stop(const char *key)
{
    struct container c;
    int i, wait_s;

    if (bf_container_find(key, &c) < 0) {
        bf_err("no such container: %s", key);
        return 1;
    }
    wait_s = c.stop_timeout + 10;
    if (bf_pid_alive(c.monitor_pid)) {
        /* The monitor owns the container: it performs the graceful stop
         * (SIGTERM -> wait -> SIGKILL) and the full cleanup. */
        kill(c.monitor_pid, SIGTERM);
        for (i = 0; i < wait_s * 10 && bf_pid_alive(c.monitor_pid); i++)
            bf_msleep(100);
        if (bf_pid_alive(c.monitor_pid)) {
            bf_warn("monitor %d did not finish, killing container pid %d", (int)c.monitor_pid,
                    (int)c.host_pid);
            kill(c.host_pid, SIGKILL);
            kill(c.monitor_pid, SIGKILL);
            for (i = 0; i < 50 && bf_pid_alive(c.monitor_pid); i++)
                bf_msleep(100);
        }
    } else if (bf_pid_alive(c.host_pid)) {
        (void)bf_signals_stop_pid(c.host_pid, c.stop_timeout);
    }
    /* If the monitor died abnormally, roll back what it left behind. */
    if (!bf_pid_alive(c.monitor_pid) && !bf_pid_alive(c.host_pid)) {
        char p[PATH_MAX];
        if (bf_snprintf(p, sizeof p, "%s/state", c.runtime_dir) == 0 && access(p, F_OK) == 0)
            bf_container_cleanup(&c);
    }
    printf("stopped %s\n", c.id);
    return 0;
}

/* ---- exec ---------------------------------------------------------------- */

static volatile pid_t g_exec_child = 0;
static void exec_sig_forward(int sig)
{
    if (g_exec_child > 0)
        kill(g_exec_child, sig);
}

int bf_lifecycle_exec(const char *key, int argc, char *const argv[])
{
    struct container c;
    bool joined_user = false;
    pid_t child;
    int status, r;

    if (argc < 1) {
        bf_err("exec: missing command");
        return 1;
    }
    r = bf_container_find(key, &c);
    if (r < 0) {
        bf_err(r == -2 ? "ambiguous container id: %s" : "no such container: %s", key);
        return 1;
    }
    if (c.state != BF_ST_RUNNING || !bf_pid_alive(c.host_pid)) {
        bf_err("container %s is not running", c.id);
        return 1;
    }
    /* Join the container's cgroup first so the new process obeys its limits. */
    if (bf_cgroup_add_pid(&c, getpid()) < 0)
        return 1;
    /* setns(): user -> mnt -> uts -> ipc -> net -> pid.  Entering the PID
     * namespace does NOT change this process's own PID; only children created
     * afterwards live inside it, hence the fork() below. */
    if (bf_ns_join(c.host_pid, &joined_user) < 0)
        return 1;

    fflush(NULL);
    child = fork();
    if (child < 0) {
        bf_err("fork: %s", strerror(errno));
        return 1;
    }
    if (child == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (joined_user && bf_userns_become_root() < 0) {
            fprintf(stderr, "boxforge: cannot become root in user namespace: %s\n", strerror(errno));
            _exit(BF_EXIT_SETUP);
        }
        if (chdir("/") < 0) {
            fprintf(stderr, "boxforge: chdir /: %s\n", strerror(errno));
            _exit(BF_EXIT_SETUP);
        }
        if (!c.opt_keep_caps && bf_ns_drop_capabilities() < 0) {
            fprintf(stderr, "boxforge: drop capabilities: %s\n", strerror(errno));
            _exit(BF_EXIT_SETUP);
        }
        bf_ns_setup_env(&c);
        execvp(argv[0], argv);
        fprintf(stderr, "boxforge: exec %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? BF_EXIT_NOTFOUND : BF_EXIT_NOEXEC);
    }
    /* Forward termination signals to the child, ignore terminal signals in parent */
    g_exec_child = child;
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, exec_sig_forward);
    signal(SIGHUP, exec_sig_forward);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            g_exec_child = 0;
            return 1;
        }
    }
    g_exec_child = 0;
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* ---- list / info --------------------------------------------------------- */

static int list_cb(const struct container *c, void *arg)
{
    const char *state = c->state == BF_ST_RUNNING && !bf_pid_alive(c->host_pid)
                            ? "dead(stale)"
                            : bf_state_name(c->state);
    char cmd[40];

    (void)arg;
    snprintf(cmd, sizeof cmd, "%.36s%s", c->cmdline, strlen(c->cmdline) > 36 ? "..." : "");
    printf("%-13s %-12s %-8d %-12s %s\n", c->id, c->name[0] ? c->name : "-", (int)c->host_pid,
           state, cmd);
    return 0;
}

int bf_lifecycle_list(void)
{
    if (bf_runtime_root_ensure() < 0)
        return 1;
    printf("%-13s %-12s %-8s %-12s %s\n", "CONTAINER ID", "NAME", "PID", "STATE", "COMMAND");
    return bf_container_foreach(list_cb, NULL) < 0 ? 1 : 0;
}

int bf_lifecycle_info(const char *key)
{
    static const char *const nss[] = {"pid", "mnt", "uts", "ipc", "net", "user"};
    struct container c;
    char path[PATH_MAX], link[PATH_MAX];
    ssize_t n;
    int r = bf_container_find(key, &c);
    size_t i;
    char when[64];
    struct tm tm;

    if (r < 0) {
        bf_err(r == -2 ? "ambiguous container id: %s" : "no such container: %s", key);
        return 1;
    }
    localtime_r(&c.created_at, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    printf("id:          %s\nname:        %s\nstate:       %s%s\n", c.id,
           c.name[0] ? c.name : "-", bf_state_name(c.state),
           (c.state == BF_ST_RUNNING && !bf_pid_alive(c.host_pid)) ? " (process gone: stale)" : "");
    printf("host pid:    %d   (PID 1 inside the container)\nmonitor pid: %d\n", (int)c.host_pid,
           (int)c.monitor_pid);
    printf("rootfs:      %s\ncommand:     %s\nhostname:    %s\ncreated:     %s\n", c.rootfs,
           c.cmdline, c.hostname, when);
    printf("mode:        %s%s%s%s\n", c.opt_init ? "init-supervisor" : "direct-exec",
           c.opt_overlay ? " +overlay" : "", c.opt_userns ? " +userns" : "",
           c.opt_net_bridge ? " +bridge-net" : "");
    printf("memory.max:  %s", c.mem_limit ? "" : "unlimited\n");
    if (c.mem_limit)
        printf("%llu bytes\n", c.mem_limit);
    if (c.cpu_percent)
        printf("cpu.max:     %u%% of one CPU\n", c.cpu_percent);
    else
        printf("cpu.max:     unlimited\n");
    printf("cgroup:      %s\n", c.cgroup_path[0] ? c.cgroup_path : "-");
    if (c.ip[0])
        printf("ip:          %s (veth %s)\n", c.ip, c.veth_host);
    if (bf_pid_alive(c.host_pid)) {
        printf("namespaces:\n");
        for (i = 0; i < sizeof nss / sizeof nss[0]; i++) {
            bf_snprintf(path, sizeof path, "/proc/%d/ns/%s", (int)c.host_pid, nss[i]);
            n = readlink(path, link, sizeof link - 1);
            if (n > 0) {
                link[n] = '\0';
                printf("  %-5s %s\n", nss[i], link);
            }
        }
    }
    return 0;
}
