/* cgroup.c - cgroup v2 handling */
#include "cgroup.h"

#include <dirent.h>
#include <errno.h>
#include <mntent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BF_CG_DIR "boxforge"

int bf_cgroup_detect_root(char *out, size_t n)
{
    FILE *f = setmntent("/proc/mounts", "r");
    struct mntent *m;
    int rc = -1;

    if (f == NULL)
        return -1;
    while ((m = getmntent(f)) != NULL) {
        if (strcmp(m->mnt_type, "cgroup2") == 0) {
            rc = bf_snprintf(out, n, "%s", m->mnt_dir);
            break;
        }
    }
    endmntent(f);
    if (rc < 0 && errno == 0)
        errno = ENOENT;
    return rc;
}

int bf_parse_memory(const char *s, unsigned long long *bytes)
{
    if (bf_parse_size(s, bytes) < 0 || *bytes < (1ULL << 20)) {
        bf_err("invalid --memory value '%s' (examples: 64M, 256M, 1G; minimum 1M)", s);
        return -1;
    }
    return 0;
}

int bf_parse_cpu_percent(const char *s, unsigned *pct)
{
    char *end = NULL;
    long v, ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    if (ncpu < 1)
        ncpu = 1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || (*end != '\0' && strcmp(end, "%") != 0) || v < 1 ||
        v > 100 * ncpu) {
        bf_err("invalid --cpu value '%s' (expected 1..%ld, percent of one CPU)", s, 100 * ncpu);
        return -1;
    }
    *pct = (unsigned)v;
    return 0;
}

static bool has_word(const char *haystack, const char *word)
{
    size_t wl = strlen(word);
    const char *p = haystack;

    while ((p = strstr(p, word)) != NULL) {
        bool start = (p == haystack || p[-1] == ' ' || p[-1] == '\n');
        bool end = (p[wl] == '\0' || p[wl] == ' ' || p[wl] == '\n');
        if (start && end)
            return true;
        p += wl;
    }
    return false;
}

static int cg_write(const char *dir, const char *file, const char *value)
{
    char path[PATH_MAX];

    if (bf_snprintf(path, sizeof path, "%s/%s", dir, file) < 0)
        return -1;
    if (bf_write_file(path, value) < 0) {
        bf_debug("write '%s' to %s: %s", value, path, strerror(errno));
        return -1;
    }
    return 0;
}

static void enable_controller(const char *dir, const char *ctrl)
{
    char avail[512], path[PATH_MAX], val[32];

    if (bf_snprintf(path, sizeof path, "%s/cgroup.controllers", dir) < 0 ||
        bf_read_file(path, avail, sizeof avail) < 0 || !has_word(avail, ctrl))
        return;
    if (bf_snprintf(val, sizeof val, "+%s", ctrl) == 0)
        (void)cg_write(dir, "cgroup.subtree_control", val); /* may already be enabled */
}

int bf_cgroup_create(struct container *c)
{
    char root[PATH_MAX], base[PATH_MAX], avail[512], path[PATH_MAX], val[64];

    if (bf_cgroup_detect_root(root, sizeof root) < 0) {
        bf_err("cgroup v2 is not mounted. Fix: sudo mount -t cgroup2 none /sys/fs/cgroup "
               "(or boot with systemd.unified_cgroup_hierarchy=1). Use --no-cgroup to skip.");
        return -1;
    }
    if (bf_snprintf(path, sizeof path, "%s/cgroup.controllers", root) < 0 ||
        bf_read_file(path, avail, sizeof avail) < 0) {
        bf_err("cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    if (c->mem_limit && !has_word(avail, "memory")) {
        bf_err("the cgroup v2 'memory' controller is not available in %s (have: %s). "
               "On a v1/hybrid host boot with systemd.unified_cgroup_hierarchy=1 (see README, "
               "WSL2 section).", root, avail);
        return -1;
    }
    if (c->cpu_percent && !has_word(avail, "cpu")) {
        bf_err("the cgroup v2 'cpu' controller is not available in %s (have: %s). "
               "On a v1/hybrid host boot with systemd.unified_cgroup_hierarchy=1.", root, avail);
        return -1;
    }

    if (bf_snprintf(base, sizeof base, "%s/%s", root, BF_CG_DIR) < 0)
        return -1;
    if (mkdir(base, 0755) < 0 && errno != EEXIST) {
        bf_err("mkdir %s: %s", base, strerror(errno));
        return -1;
    }
    enable_controller(root, "memory");
    enable_controller(root, "cpu");
    enable_controller(base, "memory");
    enable_controller(base, "cpu");

    if (bf_snprintf(c->cgroup_path, sizeof c->cgroup_path, "%s/%s", base, c->id) < 0)
        return -1;
    if (mkdir(c->cgroup_path, 0755) < 0) {
        bf_err("mkdir %s: %s", c->cgroup_path, strerror(errno));
        c->cgroup_path[0] = '\0';
        return -1;
    }
    c->cleanup.cgroup = true;

    if (c->mem_limit) {
        bf_snprintf(val, sizeof val, "%llu", c->mem_limit);
        if (cg_write(c->cgroup_path, "memory.max", val) < 0) {
            bf_err("cannot set memory.max in %s: %s", c->cgroup_path, strerror(errno));
            rmdir(c->cgroup_path);
            c->cgroup_path[0] = '\0';
            bf_cgroup_cleanup_parent();
            return -1;
        }
        (void)cg_write(c->cgroup_path, "memory.swap.max", "0"); /* make the limit real */
        (void)cg_write(c->cgroup_path, "memory.oom.group", "1");
    }
    if (c->cpu_percent) {
        c->cpu_quota = (unsigned long)c->cpu_percent * (c->cpu_period / 100);
        bf_snprintf(val, sizeof val, "%lu %lu", c->cpu_quota, c->cpu_period);
        if (cg_write(c->cgroup_path, "cpu.max", val) < 0) {
            bf_err("cannot set cpu.max in %s: %s", c->cgroup_path, strerror(errno));
            rmdir(c->cgroup_path);
            c->cgroup_path[0] = '\0';
            bf_cgroup_cleanup_parent();
            return -1;
        }
    }
    bf_info("cgroup %s ready (memory=%llu cpu=%u%%)", c->cgroup_path, c->mem_limit,
            c->cpu_percent);
    return 0;
}

int bf_cgroup_add_pid(const struct container *c, pid_t pid)
{
    char val[32];

    if (c->cgroup_path[0] == '\0')
        return 0;
    bf_snprintf(val, sizeof val, "%d", (int)pid);
    if (cg_write(c->cgroup_path, "cgroup.procs", val) < 0) {
        bf_err("cannot move pid %d into %s: %s", (int)pid, c->cgroup_path, strerror(errno));
        return -1;
    }
    return 0;
}

unsigned long bf_cgroup_oom_kills(const struct container *c)
{
    char path[PATH_MAX], buf[512];
    char *p;

    if (c->cgroup_path[0] == '\0' ||
        bf_snprintf(path, sizeof path, "%s/memory.events", c->cgroup_path) < 0 ||
        bf_read_file(path, buf, sizeof buf) < 0)
        return 0;
    p = strstr(buf, "oom_kill ");
    return p ? strtoul(p + 9, NULL, 10) : 0;
}

void bf_cgroup_cleanup_parent(void)
{
    char base[PATH_MAX], path[PATH_MAX];
    DIR *d;
    struct dirent *e;
    int children = 0;
    int retry;

    if (bf_cgroup_detect_root(base, sizeof base) < 0 ||
        bf_snprintf(path, sizeof path, "%s/" BF_CG_DIR, base) < 0)
        return;

    if (access(path, F_OK) != 0)
        return;

    /* Check if any child container directories remain */
    d = opendir(path);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            char sub[PATH_MAX];
            struct stat st;
            if (bf_snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) == 0 &&
                stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
                children++;
        }
    }
    closedir(d);

    if (children > 0)
        return; /* other containers are still running */

    /* Try removing the parent cgroup; if busy, disable subtree controllers and retry */
    for (retry = 0; retry < 10; retry++) {
        if (rmdir(path) == 0 || errno == ENOENT)
            return;
        if (errno == ENOTEMPTY)
            return;
        if (errno == EBUSY) {
            (void)cg_write(path, "cgroup.subtree_control", "-memory -cpu");
            bf_msleep(25);
            continue;
        }
        bf_debug("rmdir parent cgroup %s: %s", path, strerror(errno));
        break;
    }
}

void bf_cgroup_remove(struct container *c)
{
    char path[PATH_MAX], buf[1024];
    int i;

    if (c->cgroup_path[0] == '\0') {
        bf_cgroup_cleanup_parent();
        return;
    }

    /* Kill any lingering processes immediately */
    if (bf_snprintf(path, sizeof path, "%s/cgroup.kill", c->cgroup_path) == 0)
        (void)bf_write_file(path, "1"); /* kernel >= 5.14 */

    /* Fallback for older kernels: read cgroup.procs and kill each process */
    if (bf_snprintf(path, sizeof path, "%s/cgroup.procs", c->cgroup_path) == 0 &&
        bf_read_file(path, buf, sizeof buf) > 0) {
        char *line, *save = NULL;
        for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
            pid_t p = (pid_t)atoi(line);
            if (p > 0)
                (void)kill(p, SIGKILL);
        }
    }

    for (i = 0; i < 100; i++) {
        if (rmdir(c->cgroup_path) == 0 || errno == ENOENT) {
            c->cgroup_path[0] = '\0';
            bf_cgroup_cleanup_parent();
            return;
        }
        if (errno != EBUSY) {
            bf_warn("rmdir %s: %s", c->cgroup_path, strerror(errno));
            c->cgroup_path[0] = '\0';
            bf_cgroup_cleanup_parent();
            return;
        }
        if (i % 10 == 0 && bf_snprintf(path, sizeof path, "%s/cgroup.kill", c->cgroup_path) == 0)
            (void)bf_write_file(path, "1");
        bf_msleep(25);
    }
    bf_warn("cgroup %s still busy; leaving it for scripts/cleanup.sh", c->cgroup_path);
    bf_cgroup_cleanup_parent();
}
