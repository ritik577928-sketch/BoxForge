/* container.h - the central container descriptor, state machine and
 * on-disk runtime metadata. */
#ifndef BF_CONTAINER_H
#define BF_CONTAINER_H

#include <signal.h>
#include <time.h>

#include "boxforge.h"

typedef enum {
    BF_ST_CREATED = 0, /* descriptor + runtime dir exist              */
    BF_ST_SETUP,       /* cgroup/overlay/net prepared, child cloned   */
    BF_ST_RUNNING,     /* target application exec'd                   */
    BF_ST_STOPPING,    /* graceful stop requested                     */
    BF_ST_EXITED,      /* application finished                        */
    BF_ST_FAILED       /* setup failed                                */
} bf_state_t;

/* Tracks which resources were acquired so that a partially initialised
 * container can be rolled back safely (every flag is idempotent). */
struct bf_cleanup {
    bool runtime_dir;
    bool cgroup;
    bool overlay;
    bool net;
    bool pipes;
};

struct container {
    /* identity */
    char id[BF_ID_LEN + 1];
    char name[BF_NAME_MAX];
    char rootfs[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char cgroup_path[PATH_MAX];
    char hostname[BF_HOST_MAX];
    char cmdline[1024];
    char *argv[BF_MAX_ARGS + 1];
    int argc;

    /* runtime state */
    pid_t host_pid;    /* PID of the container's init as seen by the host */
    pid_t monitor_pid; /* the boxforge process supervising the container  */
    bf_state_t state;
    time_t created_at;
    int exit_code;
    bool oom_killed;

    /* resource limits */
    unsigned long long mem_limit; /* bytes, 0 = unlimited */
    unsigned cpu_percent;         /* 0 = unlimited        */
    unsigned long cpu_quota;      /* microseconds per period */
    unsigned long cpu_period;

    /* options */
    bool opt_init;
    bool opt_detach;
    bool opt_overlay;
    bool opt_userns;
    bool opt_keep_caps;
    bool opt_no_cgroup;
    bool opt_chroot;
    bool opt_net_bridge;
    uid_t userns_uid;
    gid_t userns_gid;
    int stop_timeout;
    char log_file[PATH_MAX];

    /* overlay */
    char overlay_dir[PATH_MAX]; /* persistent dir (--overlay-dir) or "" */
    char overlay_upper[PATH_MAX];
    char overlay_work[PATH_MAX];
    char overlay_merged[PATH_MAX];

    /* network (bridge mode) */
    char ip[32];
    char veth_host[16];
    char veth_peer[16];

    /* plumbing */
    int sync_pipe[2]; /* parent -> child: 'G' (go) or 'X' (abort) */
    int err_pipe[2];  /* child -> parent: setup/exec error text    */
    int ready_fd;     /* detach: daemon -> launcher status pipe    */
    sigset_t saved_mask;
    struct bf_cleanup cleanup;
};

typedef int (*bf_container_cb)(const struct container *c, void *arg);

void bf_container_init(struct container *c);
const char *bf_state_name(bf_state_t s);

const char *bf_runtime_root(void);
int bf_runtime_root_ensure(void);

void bf_container_gen_id(struct container *c);
int bf_container_make_runtime_dir(struct container *c);
int bf_container_set_cmdline(struct container *c);

int bf_container_save(const struct container *c);
int bf_container_load(struct container *c, const char *id);
int bf_container_find(const char *key, struct container *out);
bool bf_container_name_in_use(const char *name);
int bf_container_foreach(bf_container_cb cb, void *arg);

/* Roll back every resource recorded in c->cleanup. Safe to call repeatedly. */
void bf_container_cleanup(struct container *c);

#endif
