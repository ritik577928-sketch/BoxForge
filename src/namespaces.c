/* namespaces.c */
#include "namespaces.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int bf_ns_clone_flags(const struct container *c)
{
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;

    if (c->opt_userns)
        flags |= CLONE_NEWUSER;
    return flags;
}

int bf_ns_set_hostname(const char *name)
{
    return sethostname(name, strlen(name));
}

int bf_ns_loopback_up(void)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int saved;

    if (s < 0)
        return -1;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "lo");
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
        goto fail;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
        goto fail;
    close(s);
    return 0;
fail:
    saved = errno;
    close(s);
    errno = saved;
    return -1;
}

int bf_ns_drop_capabilities(void)
{
    /* Capabilities that allow module loading, raw hardware access, time/boot
     * control, MAC/audit tampering and (re)configuring the kernel. */
    static const int drop[] = {
        CAP_LINUX_IMMUTABLE, CAP_NET_BROADCAST, CAP_NET_ADMIN,  CAP_IPC_LOCK,
        CAP_SYS_MODULE,      CAP_SYS_RAWIO,     CAP_SYS_PTRACE, CAP_SYS_PACCT,
        CAP_SYS_ADMIN,       CAP_SYS_BOOT,      CAP_SYS_NICE,   CAP_SYS_RESOURCE,
        CAP_SYS_TIME,        CAP_SYS_TTY_CONFIG, CAP_AUDIT_CONTROL, CAP_MAC_OVERRIDE,
        CAP_MAC_ADMIN,       CAP_SYSLOG,        CAP_WAKE_ALARM, CAP_BLOCK_SUSPEND,
    };
    size_t i;

    for (i = 0; i < sizeof drop / sizeof drop[0]; i++) {
        if (prctl(PR_CAPBSET_DROP, drop[i], 0, 0, 0) < 0 && errno != EINVAL)
            return -1;
    }
    return 0;
}

void bf_ns_setup_env(const struct container *c)
{
    char term[64] = "";
    const char *t = getenv("TERM");

    if (t != NULL)
        snprintf(term, sizeof term, "%s", t);
    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("HOSTNAME", c->hostname, 1);
    setenv("container", "boxforge", 1);
    if (term[0] != '\0')
        setenv("TERM", term, 1);
}

static bool same_ns(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_ino == sb.st_ino &&
           sa.st_dev == sb.st_dev;
}

int bf_ns_join(pid_t pid, bool *joined_user)
{
    static const struct {
        const char *name;
        int type;
    } nsl[] = {{"user", CLONE_NEWUSER}, {"mnt", CLONE_NEWNS}, {"uts", CLONE_NEWUTS},
               {"ipc", CLONE_NEWIPC},   {"net", CLONE_NEWNET}, {"pid", CLONE_NEWPID}};
    int fds[6], i, rc = 0;
    char path[PATH_MAX], self[PATH_MAX];

    *joined_user = false;
    for (i = 0; i < 6; i++)
        fds[i] = -1;

    /* Open everything first: once we enter the mount namespace the host's
     * /proc is no longer reachable. */
    for (i = 0; i < 6; i++) {
        bf_snprintf(path, sizeof path, "/proc/%d/ns/%s", (int)pid, nsl[i].name);
        bf_snprintf(self, sizeof self, "/proc/self/ns/%s", nsl[i].name);
        if (access(path, F_OK) < 0) {
            if (nsl[i].type == CLONE_NEWUSER)
                continue; /* kernel without user namespaces */
            bf_err("cannot open %s: %s", path, strerror(errno));
            rc = -1;
            goto out;
        }
        if (same_ns(path, self))
            continue; /* container shares this namespace with us (e.g. no userns) */
        fds[i] = open(path, O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) {
            bf_err("open %s: %s", path, strerror(errno));
            rc = -1;
            goto out;
        }
    }
    for (i = 0; i < 6; i++) {
        if (fds[i] < 0)
            continue;
        if (setns(fds[i], nsl[i].type) < 0) {
            bf_err("setns(%s): %s", nsl[i].name, strerror(errno));
            rc = -1;
            goto out;
        }
        if (nsl[i].type == CLONE_NEWUSER)
            *joined_user = true;
    }
out:
    for (i = 0; i < 6; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    return rc;
}
