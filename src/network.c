/* network.c - bridge/veth/NAT networking (uses iproute2 + iptables on the host) */
#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dirent.h>

static int run_ip(char *a, char *b, char *c, char *d, char *e, char *f, char *g, char *h)
{
    /* unused trailing arguments are NULL, which terminates argv early */
    char *argv[] = {"ip", a, b, c, d, e, f, g, h, NULL};
    int rc = bf_run_cmd(argv, false);

    if (rc != 0)
        bf_err("`ip %s %s %s ...` failed (status %d)", a, b ? b : "", c ? c : "", rc);
    return rc == 0 ? 0 : -1;
}

static int net_lock(void)
{
    char path[PATH_MAX];
    int fd;

    if (bf_snprintf(path, sizeof path, "%s/net.lock", bf_runtime_root()) < 0)
        return -1;
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void net_unlock(int fd)
{
    if (fd >= 0)
        close(fd); /* closing releases the flock */
}

static bool bridge_exists(void) { return access("/sys/class/net/" BF_BRIDGE, F_OK) == 0; }

static int bridge_members(void)
{
    DIR *d = opendir("/sys/class/net/" BF_BRIDGE "/brif");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static void marker_path(char *out, size_t n, const char *name)
{
    bf_snprintf(out, n, "%s/%s", bf_runtime_root(), name);
}

/* iptables helper: add rule only if `-C` says it does not exist yet. */
static int ipt_ensure(bool insert, char *const rule[], int nrule, const char *table)
{
    char *argv[32];
    int i, n = 0;

    argv[n++] = "iptables";
    argv[n++] = "-w";
    argv[n++] = "-t";
    argv[n++] = (char *)table;
    argv[n++] = "-C";
    for (i = 0; i < nrule && n < 28; i++)
        argv[n++] = rule[i];
    argv[n] = NULL;
    if (bf_run_cmd(argv, true) == 0)
        return 0;
    argv[4] = insert ? "-I" : "-A";
    return bf_run_cmd(argv, false) == 0 ? 0 : -1;
}

static void ipt_delete_all(char *const rule[], int nrule, const char *table)
{
    char *argv[32];
    int i, n;

    for (;;) {
        n = 0;
        argv[n++] = "iptables";
        argv[n++] = "-w";
        argv[n++] = "-t";
        argv[n++] = (char *)table;
        argv[n++] = "-C";
        for (i = 0; i < nrule && n < 28; i++)
            argv[n++] = rule[i];
        argv[n] = NULL;
        if (bf_run_cmd(argv, true) != 0)
            break; /* rule no longer exists */

        argv[4] = "-D";
        (void)bf_run_cmd(argv, true);
    }
}

static char *const R_NAT[] = {"POSTROUTING", "-s", BF_SUBNET_PREFIX "0/24", "!", "-o",
                              BF_BRIDGE,     "-j", "MASQUERADE"};
static char *const R_FWD_IN[] = {"FORWARD", "-i", BF_BRIDGE, "-j", "ACCEPT"};
static char *const R_FWD_OUT[] = {"FORWARD", "-o", BF_BRIDGE, "-m", "conntrack", "--ctstate",
                                  "RELATED,ESTABLISHED", "-j", "ACCEPT"};

static int ensure_bridge_locked(void)
{
    char mk[PATH_MAX], buf[16];

    if (!bridge_exists()) {
        if (run_ip("link", "add", "name", BF_BRIDGE, "type", "bridge", NULL, NULL) < 0 ||
            run_ip("addr", "add", BF_BRIDGE_IP "/24", "dev", BF_BRIDGE, NULL, NULL, NULL) < 0 ||
            run_ip("link", "set", BF_BRIDGE, "up", NULL, NULL, NULL, NULL) < 0)
            return -1;
        marker_path(mk, sizeof mk, "bridge.created");
        int fd = open(mk, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0)
            close(fd);
    }
    if (bf_read_file("/proc/sys/net/ipv4/ip_forward", buf, sizeof buf) >= 0 && buf[0] == '0') {
        marker_path(mk, sizeof mk, "ip_forward.prev");
        if (!bf_path_exists(mk)) {
            int fd = open(mk, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) {
                (void)bf_write_all(fd, "0", 1);
                close(fd);
            }
        }
        if (bf_write_file("/proc/sys/net/ipv4/ip_forward", "1") < 0) {
            bf_err("cannot enable IPv4 forwarding: %s", strerror(errno));
            return -1;
        }
    }
    if (!bf_cmd_available("iptables")) {
        bf_warn("iptables not found: container will reach the host but not the Internet");
        return 0;
    }
    if (ipt_ensure(false, R_NAT, 8, "nat") < 0 || ipt_ensure(true, R_FWD_IN, 5, "filter") < 0 ||
        ipt_ensure(true, R_FWD_OUT, 9, "filter") < 0) {
        bf_err("could not install NAT/forwarding rules");
        return -1;
    }
    return 0;
}

struct ip_scan {
    unsigned used[256];
    const char *self;
};

static int ip_scan_cb(const struct container *c, void *arg)
{
    struct ip_scan *s = arg;
    unsigned a, b, cc, d;

    if (strcmp(c->id, s->self) != 0 && sscanf(c->ip, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && d < 256)
        s->used[d] = 1;
    return 0;
}

int bf_net_host_prepare(struct container *c)
{
    static struct ip_scan scan;
    int fd, host, rc;

    if (!bf_cmd_available("ip")) {
        bf_err("--net bridge needs the 'ip' command: sudo apt install iproute2");
        return -1;
    }
    memset(&scan, 0, sizeof scan);
    scan.self = c->id;
    fd = net_lock();
    if (fd < 0) {
        bf_err("cannot take network lock: %s", strerror(errno));
        return -1;
    }
    (void)bf_container_foreach(ip_scan_cb, &scan);
    for (host = 2; host < 255 && scan.used[host]; host++)
        ;
    if (host >= 255) {
        net_unlock(fd);
        bf_err("no free container IP in " BF_SUBNET_PREFIX "0/24");
        return -1;
    }
    bf_snprintf(c->ip, sizeof c->ip, BF_SUBNET_PREFIX "%d", host);
    bf_snprintf(c->veth_host, sizeof c->veth_host, "bfh%.8s", c->id);
    bf_snprintf(c->veth_peer, sizeof c->veth_peer, "bfc%.8s", c->id);
    c->cleanup.net = true;
    (void)bf_container_save(c); /* reserve the IP for concurrent runs */
    rc = ensure_bridge_locked();
    net_unlock(fd);
    return rc;
}

int bf_net_host_attach(struct container *c)
{
    char pid[16];

    bf_snprintf(pid, sizeof pid, "%d", (int)c->host_pid);
    if (run_ip("link", "add", c->veth_host, "type", "veth", "peer", "name", c->veth_peer) < 0)
        return -1;
    if (run_ip("link", "set", c->veth_peer, "netns", pid, NULL, NULL, NULL) < 0 ||
        run_ip("link", "set", c->veth_host, "master", BF_BRIDGE, NULL, NULL, NULL) < 0 ||
        run_ip("link", "set", c->veth_host, "up", NULL, NULL, NULL, NULL) < 0) {
        /* Rollback immediately on failure */
        (void)run_ip("link", "del", c->veth_host, NULL, NULL, NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static void fill_addr(struct sockaddr *sa, const char *ip)
{
    struct sockaddr_in in;

    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &in.sin_addr);
    memcpy(sa, &in, sizeof in);
}

int bf_net_child_configure(const struct container *c)
{
    struct ifreq ifr;
    struct rtentry rt;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int saved;

    if (s < 0)
        return -1;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", c->veth_peer);
    snprintf(ifr.ifr_newname, sizeof ifr.ifr_newname, "eth0");
    if (ioctl(s, SIOCSIFNAME, &ifr) < 0)
        goto fail;

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "eth0");
    fill_addr(&ifr.ifr_addr, c->ip);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0)
        goto fail;
    fill_addr(&ifr.ifr_netmask, "255.255.255.0");
    if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
        goto fail;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
        goto fail;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
        goto fail;

    memset(&rt, 0, sizeof rt);
    fill_addr(&rt.rt_dst, "0.0.0.0");
    fill_addr(&rt.rt_genmask, "0.0.0.0");
    fill_addr(&rt.rt_gateway, BF_BRIDGE_IP);
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    if (ioctl(s, SIOCADDRT, &rt) < 0)
        goto fail;
    close(s);
    return 0;
fail:
    saved = errno;
    close(s);
    errno = saved;
    return -1;
}

static int active_bridge_containers_cb(const struct container *c, void *arg)
{
    int *count = arg;
    if (c->opt_net_bridge && (c->state == BF_ST_SETUP || c->state == BF_ST_RUNNING))
        (*count)++;
    return 0;
}

void bf_net_teardown(struct container *c)
{
    char path[PATH_MAX], mk[PATH_MAX], buf[16];
    int fd;

    fd = net_lock();
    if (fd < 0)
        return;

    if (c->veth_host[0] != '\0') {
        snprintf(path, sizeof path, "/sys/class/net/%s", c->veth_host);
        if (access(path, F_OK) == 0)
            (void)run_ip("link", "del", c->veth_host, NULL, NULL, NULL, NULL, NULL);
        c->veth_host[0] = '\0';
    }
    if (c->veth_peer[0] != '\0') {
        snprintf(path, sizeof path, "/sys/class/net/%s", c->veth_peer);
        if (access(path, F_OK) == 0)
            (void)run_ip("link", "del", c->veth_peer, NULL, NULL, NULL, NULL, NULL);
        c->veth_peer[0] = '\0';
    }

    marker_path(mk, sizeof mk, "bridge.created");
    if (bridge_exists() && bf_path_exists(mk)) {
        int active = 0;
        (void)bf_container_foreach(active_bridge_containers_cb, &active);
        if (active <= 1 && bridge_members() == 0) {
            if (bf_cmd_available("iptables")) {
                ipt_delete_all(R_NAT, 8, "nat");
                ipt_delete_all(R_FWD_IN, 5, "filter");
                ipt_delete_all(R_FWD_OUT, 9, "filter");
            }
            (void)run_ip("link", "del", BF_BRIDGE, NULL, NULL, NULL, NULL, NULL);
            unlink(mk);
            marker_path(mk, sizeof mk, "ip_forward.prev");
            if (bf_read_file(mk, buf, sizeof buf) >= 0 && buf[0] == '0')
                (void)bf_write_file("/proc/sys/net/ipv4/ip_forward", "0");
            unlink(mk);
        }
    }
    net_unlock(fd);
}
