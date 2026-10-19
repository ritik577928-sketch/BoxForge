/* userns.c - uid/gid mapping.
 *
 * Mapping written by the (root) parent:   container 0  ->  host <uid>  (1 id)
 * Process owner on the host therefore becomes <uid>, not root. */
#include "userns.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool bf_userns_supported(void)
{
    struct stat st;
    return stat("/proc/self/ns/user", &st) == 0;
}

void bf_userns_choose_ids(struct container *c)
{
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
    uid_t uid = c->userns_uid;
    gid_t gid = c->userns_gid;

    if (uid == (uid_t)-1 || uid == 0) {
        uid = (su && atoi(su) > 0) ? (uid_t)atoi(su) : 65534;
    }
    if (gid == (gid_t)-1 || gid == 0) {
        gid = (sg && atoi(sg) > 0) ? (gid_t)atoi(sg) : (gid_t)uid;
    }
    c->userns_uid = uid;
    c->userns_gid = gid;
}

int bf_userns_write_maps(const struct container *c, pid_t child)
{
    char path[64], map[64];

    /* 1. setgroups must be "deny" before an unprivileged-style gid_map; we
     *    keep it for defence in depth even though the writer is root. */
    bf_snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (bf_write_file(path, "deny") < 0) {
        bf_err("write %s: %s", path, strerror(errno));
        return -1;
    }
    bf_snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    bf_snprintf(map, sizeof map, "0 %u 1\n", (unsigned)c->userns_gid);
    if (bf_write_file(path, map) < 0) {
        bf_err("write %s: %s", path, strerror(errno));
        return -1;
    }
    bf_snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    bf_snprintf(map, sizeof map, "0 %u 1\n", (unsigned)c->userns_uid);
    if (bf_write_file(path, map) < 0) {
        bf_err("write %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

#include <grp.h>

int bf_userns_become_root(void)
{
    (void)setgroups(0, NULL);
    if (setresgid(0, 0, 0) < 0)
        return -1;
    return setresuid(0, 0, 0);
}
