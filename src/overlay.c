/* overlay.c */
#include "overlay.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static bool path_ok(const char *p)
{
    /* ',' ':' and '\\' are special in overlayfs mount options */
    return strpbrk(p, ",:\\") == NULL;
}

int bf_overlay_prepare(struct container *c)
{
    char base[PATH_MAX];

    if (access("/proc/filesystems", R_OK) == 0) {
        char buf[4096];
        if (bf_read_file("/proc/filesystems", buf, sizeof buf) >= 0 && strstr(buf, "overlay") == NULL) {
            bf_err("this kernel has no overlayfs support (try: sudo modprobe overlay)");
            return -1;
        }
    }
    if (c->overlay_dir[0] != '\0') {
        if (bf_mkdir_p(c->overlay_dir, 0755) < 0) {
            bf_err("cannot create overlay dir %s: %s", c->overlay_dir, strerror(errno));
            return -1;
        }
        snprintf(base, sizeof base, "%s", c->overlay_dir);
    } else if (bf_snprintf(base, sizeof base, "%s/overlay", c->runtime_dir) < 0) {
        return -1;
    }
    if (bf_snprintf(c->overlay_upper, sizeof c->overlay_upper, "%s/upper", base) < 0 ||
        bf_snprintf(c->overlay_work, sizeof c->overlay_work, "%s/work", base) < 0 ||
        bf_snprintf(c->overlay_merged, sizeof c->overlay_merged, "%s/merged", c->runtime_dir) < 0)
        return -1;
    if (!path_ok(c->rootfs) || !path_ok(c->overlay_upper) || !path_ok(c->overlay_work)) {
        bf_err("overlay paths must not contain ',' ':' or '\\'");
        return -1;
    }
    c->cleanup.overlay = true;
    if (bf_mkdir_p(c->overlay_upper, 0755) < 0 || bf_mkdir_p(c->overlay_work, 0700) < 0 ||
        bf_mkdir_p(c->overlay_merged, 0755) < 0) {
        bf_err("cannot create overlay directories: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int bf_overlay_mount(const struct container *c)
{
    char opts[3 * PATH_MAX + 64];

    if (bf_snprintf(opts, sizeof opts, "lowerdir=%s,upperdir=%s,workdir=%s%s", c->rootfs,
                    c->overlay_upper, c->overlay_work, c->opt_userns ? ",userxattr" : "") < 0)
        return -1;
    if (mount("overlay", c->overlay_merged, "overlay", 0, opts) < 0) {
        bf_err("mount overlay on %s: %s (lower=%s)", c->overlay_merged, strerror(errno), c->rootfs);
        return -1;
    }
    return 0;
}

void bf_overlay_cleanup(struct container *c)
{
    /* merged lives in the runtime dir (removed with it).  The work dir is
     * scratch space and always removed; the upper dir survives only when the
     * user asked for a persistent --overlay-dir. */
    if (c->overlay_merged[0] != '\0') {
        (void)umount2(c->overlay_merged, MNT_DETACH);
        (void)rmdir(c->overlay_merged);
        c->overlay_merged[0] = '\0';
    }
    if (c->overlay_work[0] != '\0') {
        (void)bf_rm_rf(c->overlay_work);
        c->overlay_work[0] = '\0';
    }
    if (c->overlay_dir[0] == '\0' && c->overlay_upper[0] != '\0') {
        (void)bf_rm_rf(c->overlay_upper);
        c->overlay_upper[0] = '\0';
    }
}
