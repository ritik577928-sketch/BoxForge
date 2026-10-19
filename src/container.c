/* container.c - container descriptor, metadata persistence, rollback */
#include "container.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cgroup.h"
#include "network.h"
#include "overlay.h"

static const char *const state_names[] = {"created", "setup", "running",
                                          "stopping", "exited", "failed"};

const char *bf_state_name(bf_state_t s)
{
    return (s >= BF_ST_CREATED && s <= BF_ST_FAILED) ? state_names[s] : "unknown";
}

void bf_container_init(struct container *c)
{
    memset(c, 0, sizeof *c);
    c->sync_pipe[0] = c->sync_pipe[1] = -1;
    c->err_pipe[0] = c->err_pipe[1] = -1;
    c->ready_fd = -1;
    c->state = BF_ST_CREATED;
    c->cpu_period = 100000;
    c->stop_timeout = BF_DEFAULT_STOP_TIMEOUT;
    c->created_at = time(NULL);
    snprintf(c->hostname, sizeof c->hostname, "boxforge");
}

const char *bf_runtime_root(void)
{
    const char *e = getenv("BOXFORGE_RUNTIME_DIR");
    return (e != NULL && *e == '/') ? e : BF_DEFAULT_RUNTIME_DIR;
}

/* The runtime directory holds privileged state: it must exist, be a real
 * directory owned by the current (root) user and not writable by others. */
int bf_runtime_root_ensure(void)
{
    const char *root = bf_runtime_root();
    struct stat st;

    if (bf_mkdir_p(root, 0700) < 0) {
        bf_err("cannot create runtime directory %s: %s", root, strerror(errno));
        return -1;
    }
    if (lstat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        bf_err("runtime path %s is not a directory", root);
        return -1;
    }
    if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        bf_err("runtime directory %s must be owned by uid %d and not group/world writable",
               root, (int)geteuid());
        return -1;
    }
    return 0;
}

void bf_container_gen_id(struct container *c) { bf_random_hex(c->id, BF_ID_LEN); }

int bf_container_make_runtime_dir(struct container *c)
{
    int tries;

    if (bf_runtime_root_ensure() < 0)
        return -1;
    for (tries = 0; tries < 8; tries++) {
        if (bf_snprintf(c->runtime_dir, sizeof c->runtime_dir, "%s/%s", bf_runtime_root(),
                        c->id) < 0)
            return -1;
        if (mkdir(c->runtime_dir, 0700) == 0) {
            c->cleanup.runtime_dir = true;
            return 0;
        }
        if (errno != EEXIST) {
            bf_err("mkdir %s: %s", c->runtime_dir, strerror(errno));
            return -1;
        }
        bf_container_gen_id(c);
    }
    bf_err("could not allocate a unique container id");
    return -1;
}

int bf_container_set_cmdline(struct container *c)
{
    size_t used = 0;
    int i;

    c->cmdline[0] = '\0';
    for (i = 0; i < c->argc; i++) {
        int n = snprintf(c->cmdline + used, sizeof c->cmdline - used, "%s%s", i ? " " : "",
                         c->argv[i]);
        if (n < 0 || (size_t)n >= sizeof c->cmdline - used)
            break;
        used += (size_t)n;
    }
    for (char *p = c->cmdline; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    return 0;
}

int bf_container_save(const struct container *c)
{
    char tmp[PATH_MAX], fin[PATH_MAX];
    FILE *f;

    if (bf_snprintf(tmp, sizeof tmp, "%s/state.tmp", c->runtime_dir) < 0 ||
        bf_snprintf(fin, sizeof fin, "%s/state", c->runtime_dir) < 0)
        return -1;
    f = fopen(tmp, "w");
    if (f == NULL) {
        bf_err("cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    fprintf(f, "id=%s\nname=%s\nrootfs=%s\npid=%d\nmonitor_pid=%d\nstate=%s\n", c->id,
            c->name, c->rootfs, (int)c->host_pid, (int)c->monitor_pid, bf_state_name(c->state));
    fprintf(f, "hostname=%s\ncgroup=%s\nmemory=%llu\ncpu=%u\ncreated=%ld\n", c->hostname,
            c->cgroup_path, c->mem_limit, c->cpu_percent, (long)c->created_at);
    fprintf(f, "init=%d\noverlay=%d\nuserns=%d\nnet_bridge=%d\nstop_timeout=%d\nkeep_caps=%d\n",
            c->opt_init, c->opt_overlay, c->opt_userns, c->opt_net_bridge, c->stop_timeout,
            c->opt_keep_caps);
    fprintf(f, "overlay_dir=%s\nip=%s\nveth=%s\ncmdline=%s\n", c->overlay_dir, c->ip,
            c->veth_host, c->cmdline);
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    if (rename(tmp, fin) < 0) {
        bf_err("rename %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

static void apply_kv(struct container *c, const char *k, const char *v)
{
#define COPY(field)                                                                              \
    do {                                                                                         \
        snprintf(c->field, sizeof c->field, "%s", v);                                            \
    } while (0)
    if (!strcmp(k, "id"))
        COPY(id);
    else if (!strcmp(k, "name"))
        COPY(name);
    else if (!strcmp(k, "rootfs"))
        COPY(rootfs);
    else if (!strcmp(k, "pid"))
        c->host_pid = (pid_t)atoi(v);
    else if (!strcmp(k, "monitor_pid"))
        c->monitor_pid = (pid_t)atoi(v);
    else if (!strcmp(k, "state")) {
        for (int i = BF_ST_CREATED; i <= BF_ST_FAILED; i++)
            if (!strcmp(v, state_names[i]))
                c->state = (bf_state_t)i;
    } else if (!strcmp(k, "hostname"))
        COPY(hostname);
    else if (!strcmp(k, "cgroup"))
        COPY(cgroup_path);
    else if (!strcmp(k, "memory"))
        c->mem_limit = strtoull(v, NULL, 10);
    else if (!strcmp(k, "cpu"))
        c->cpu_percent = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "created"))
        c->created_at = (time_t)atol(v);
    else if (!strcmp(k, "init"))
        c->opt_init = atoi(v) != 0;
    else if (!strcmp(k, "overlay"))
        c->opt_overlay = atoi(v) != 0;
    else if (!strcmp(k, "userns"))
        c->opt_userns = atoi(v) != 0;
    else if (!strcmp(k, "net_bridge"))
        c->opt_net_bridge = atoi(v) != 0;
    else if (!strcmp(k, "stop_timeout"))
        c->stop_timeout = atoi(v);
    else if (!strcmp(k, "keep_caps"))
        c->opt_keep_caps = atoi(v) != 0;
    else if (!strcmp(k, "overlay_dir"))
        COPY(overlay_dir);
    else if (!strcmp(k, "ip"))
        COPY(ip);
    else if (!strcmp(k, "veth"))
        COPY(veth_host);
    else if (!strcmp(k, "cmdline"))
        COPY(cmdline);
#undef COPY
}

int bf_container_load(struct container *c, const char *id)
{
    char path[PATH_MAX];
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    bf_container_init(c);
    if (bf_snprintf(path, sizeof path, "%s/%s/state", bf_runtime_root(), id) < 0)
        return -1;
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    while ((n = getline(&line, &cap, f)) >= 0) {
        char *eq;
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';
        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        apply_kv(c, line, eq + 1);
    }
    free(line);
    fclose(f);
    if (bf_snprintf(c->runtime_dir, sizeof c->runtime_dir, "%s/%s", bf_runtime_root(), id) < 0)
        return -1;
    /* A loaded descriptor may own anything its owner created: let cleanup
     * try everything (all steps are idempotent). */
    c->cleanup = (struct bf_cleanup){.runtime_dir = true,
                                     .cgroup = c->cgroup_path[0] != '\0',
                                     .overlay = c->opt_overlay,
                                     .net = c->opt_net_bridge,
                                     .pipes = false};
    if (c->overlay_dir[0] != '\0')
        (void)bf_snprintf(c->overlay_work, sizeof c->overlay_work, "%s/work", c->overlay_dir);
    snprintf(c->veth_peer, sizeof c->veth_peer, "%s", "");
    return 0;
}

int bf_container_foreach(bf_container_cb cb, void *arg)
{
    DIR *d = opendir(bf_runtime_root());
    struct dirent *e;
    int rc = 0;

    if (d == NULL)
        return errno == ENOENT ? 0 : -1;
    while ((e = readdir(d)) != NULL) {
        struct container c;
        if (strlen(e->d_name) != BF_ID_LEN)
            continue;
        if (bf_container_load(&c, e->d_name) < 0)
            continue;
        rc = cb(&c, arg);
        if (rc != 0)
            break;
    }
    closedir(d);
    return rc;
}

struct find_ctx {
    const char *key;
    struct container found;
    int matches;
};

static int find_cb(const struct container *c, void *arg)
{
    struct find_ctx *f = arg;
    size_t klen = strlen(f->key);

    if (!strcmp(c->id, f->key) || (c->name[0] && !strcmp(c->name, f->key))) {
        f->found = *c;
        f->matches = 1;
        return 1; /* exact match wins */
    }
    if (klen >= 3 && !strncmp(c->id, f->key, klen)) {
        f->found = *c;
        f->matches++;
    }
    return 0;
}

int bf_container_find(const char *key, struct container *out)
{
    static struct find_ctx ctx; /* large: keep off the stack */

    memset(&ctx, 0, sizeof ctx);
    ctx.key = key;
    if (bf_container_foreach(find_cb, &ctx) < 0 && ctx.matches == 0)
        return -1;
    if (ctx.matches == 0) {
        errno = ENOENT;
        return -1;
    }
    if (ctx.matches > 1) {
        errno = EEXIST;
        return -2;
    }
    *out = ctx.found;
    return 0;
}

static int name_cb(const struct container *c, void *arg)
{
    return c->name[0] != '\0' && strcmp(c->name, (const char *)arg) == 0;
}

bool bf_container_name_in_use(const char *name)
{
    return bf_container_foreach(name_cb, (void *)name) == 1;
}

void bf_container_cleanup(struct container *c)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (c->sync_pipe[i] >= 0)
            close(c->sync_pipe[i]);
        if (c->err_pipe[i] >= 0)
            close(c->err_pipe[i]);
        c->sync_pipe[i] = c->err_pipe[i] = -1;
    }
    c->cleanup.pipes = false;

    if (c->cleanup.net) {
        bf_net_teardown(c);
        c->cleanup.net = false;
    }
    if (c->cleanup.cgroup) {
        bf_cgroup_remove(c);
        c->cleanup.cgroup = false;
    } else {
        bf_cgroup_cleanup_parent();
    }
    if (c->cleanup.overlay) {
        bf_overlay_cleanup(c);
        c->cleanup.overlay = false;
    }
    if (c->rootfs[0] != '\0') {
        char old[PATH_MAX];
        if (bf_snprintf(old, sizeof old, "%s/.pivot_old", c->rootfs) == 0)
            (void)rmdir(old);
    }
    if (c->cleanup.runtime_dir) {
        if (bf_rm_rf(c->runtime_dir) < 0)
            bf_warn("could not remove %s: %s", c->runtime_dir, strerror(errno));
        c->cleanup.runtime_dir = false;
    }
}
