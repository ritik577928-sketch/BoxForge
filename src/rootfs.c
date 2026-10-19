/* rootfs.c - mount setup inside the new mount namespace */
#include "rootfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define OLD_ROOT_NAME ".pivot_old"

int bf_rootfs_resolve(struct container *c, const char *path)
{
    char resolved[PATH_MAX], probe[PATH_MAX];
    struct stat st;

    if (realpath(path, resolved) == NULL) {
        bf_err("rootfs '%s': %s", path, strerror(errno));
        return -1;
    }
    if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        bf_err("rootfs '%s' is not a directory", resolved);
        return -1;
    }
    if (bf_snprintf(c->rootfs, sizeof c->rootfs, "%s", resolved) < 0) {
        bf_err("rootfs path too long");
        return -1;
    }
    /* Early, friendly check that the command exists inside the rootfs. */
    if (c->argc > 0 && c->argv[0][0] == '/') {
        if (bf_snprintf(probe, sizeof probe, "%s%s", c->rootfs, c->argv[0]) < 0 ||
            lstat(probe, &st) < 0) {
            bf_err("command '%s' not found inside rootfs %s", c->argv[0], c->rootfs);
            return -1;
        }
    }
    return 0;
}

int bf_rootfs_make_private(void)
{
    return mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
}

static int touch_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    return close(fd);
}

static void mklink(const char *target, const char *dir, const char *name)
{
    char path[PATH_MAX];

    if (bf_snprintf(path, sizeof path, "%s/%s", dir, name) < 0 || symlink(target, path) < 0)
        bf_debug("symlink %s/%s: %s", dir, name, strerror(errno));
}

static int setup_dev(const char *root)
{
    static const struct {
        const char *name;
        bool required;
    } devs[] = {{"null", true},    {"zero", true},    {"full", true},
                {"random", true},  {"urandom", true}, {"tty", false}};
    char dev[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];
    size_t i;

    if (bf_snprintf(dev, sizeof dev, "%s/dev", root) < 0)
        return -1;
    if (bf_mkdir_p(dev, 0755) < 0)
        return -1;
    /* A private tmpfs: nothing from the host /dev is visible except the few
     * device nodes bind-mounted below. */
    if (mount("tmpfs", dev, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=64k") < 0) {
        bf_err("mount tmpfs on %s: %s", dev, strerror(errno));
        return -1;
    }
    for (i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        if (bf_snprintf(src, sizeof src, "/dev/%s", devs[i].name) < 0 ||
            bf_snprintf(dst, sizeof dst, "%s/%s", dev, devs[i].name) < 0)
            return -1;
        if (touch_file(dst) < 0 || mount(src, dst, NULL, MS_BIND, NULL) < 0) {
            if (devs[i].required) {
                bf_err("cannot provide /dev/%s: %s", devs[i].name, strerror(errno));
                return -1;
            }
            bf_debug("optional /dev/%s unavailable: %s", devs[i].name, strerror(errno));
        }
    }
    if (bf_snprintf(dst, sizeof dst, "%s/shm", dev) < 0 || mkdir(dst, 01777) < 0 ||
        chmod(dst, 01777) < 0 ||
        mount("tmpfs", dst, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=16m") < 0)
        bf_warn("could not set up /dev/shm: %s", strerror(errno));

    mklink("/proc/self/fd", dev, "fd");
    mklink("/proc/self/fd/0", dev, "stdin");
    mklink("/proc/self/fd/1", dev, "stdout");
    mklink("/proc/self/fd/2", dev, "stderr");
    return 0;
}

/* Best-effort hardening of /proc: read-only /proc/sys etc., masked kcore. */
static void harden_proc(const char *proc)
{
    static const char *const ro[] = {"sys", "sysrq-trigger", "irq", "bus"};
    static const char *const mask[] = {"kcore", "keys", "timer_list", "sched_debug"};
    char p[PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof ro / sizeof ro[0]; i++) {
        if (bf_snprintf(p, sizeof p, "%s/%s", proc, ro[i]) < 0 || access(p, F_OK) < 0)
            continue;
        if (mount(p, p, NULL, MS_BIND | MS_REC, NULL) == 0)
            mount(NULL, p, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  NULL);
        else
            bf_debug("cannot protect %s: %s", p, strerror(errno));
    }
    for (i = 0; i < sizeof mask / sizeof mask[0]; i++) {
        if (bf_snprintf(p, sizeof p, "%s/%s", proc, mask[i]) < 0 || access(p, F_OK) < 0)
            continue;
        if (mount("/dev/null", p, NULL, MS_BIND, NULL) < 0)
            bf_debug("cannot mask %s: %s", p, strerror(errno));
    }
}

static int mount_proc(const char *root)
{
    char proc[PATH_MAX];

    if (bf_snprintf(proc, sizeof proc, "%s/proc", root) < 0 || bf_mkdir_p(proc, 0555) < 0)
        return -1;
    /* Mounted from inside the new PID namespace, so it shows the container's view. */
    if (mount("proc", proc, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        bf_err("mount proc on %s: %s", proc, strerror(errno));
        return -1;
    }
    harden_proc(proc);
    return 0;
}

int bf_rootfs_prepare(const char *root)
{
    /* pivot_root requires new_root to be a mount point: bind it onto itself. */
    if (mount(root, root, NULL, MS_BIND | MS_REC, NULL) < 0) {
        bf_err("bind mount %s: %s", root, strerror(errno));
        return -1;
    }
    if (setup_dev(root) < 0)
        return -1;
    return mount_proc(root);
}

int bf_rootfs_pivot(const char *root)
{
    char old[PATH_MAX];

    if (bf_snprintf(old, sizeof old, "%s/" OLD_ROOT_NAME, root) < 0)
        return -1;
    if (mkdir(old, 0700) < 0 && errno != EEXIST) {
        if (errno != EACCES && errno != EPERM && errno != EROFS) {
            bf_err("mkdir %s: %s", old, strerror(errno));
            return -1;
        }
        /* The rootfs is not writable for us (typical with a user namespace:
         * container root is an unprivileged host uid).  Use the stacked form
         * pivot_root(".", "."), which needs no put_old directory, then detach
         * the old root that is now mounted on top of the new one. */
        bf_debug("cannot create %s (%s): using pivot_root(\".\", \".\")", old, strerror(errno));
        if (chdir(root) < 0 || syscall(SYS_pivot_root, ".", ".") < 0) {
            bf_err("pivot_root(%s): %s", root, strerror(errno));
            return -1;
        }
        if (umount2(".", MNT_DETACH) < 0) {
            bf_err("umount old root: %s", strerror(errno));
            return -1;
        }
        return chdir("/");
    }
    if (syscall(SYS_pivot_root, root, old) < 0) {
        bf_err("pivot_root(%s): %s", root, strerror(errno));
        rmdir(old);
        return -1;
    }
    if (chdir("/") < 0)
        return -1;
    if (umount2("/" OLD_ROOT_NAME, MNT_DETACH) < 0) {
        bf_err("umount old root: %s", strerror(errno));
        return -1;
    }
    for (int retry = 0; retry < 5; retry++) {
        if (rmdir("/" OLD_ROOT_NAME) == 0 || errno == ENOENT)
            break;
        bf_msleep(10);
    }
    return 0;
}

int bf_rootfs_chroot(const char *root)
{
    if (chdir(root) < 0 || chroot(".") < 0)
        return -1;
    return chdir("/");
}
