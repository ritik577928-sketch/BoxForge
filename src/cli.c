/* cli.c - boxforge run|exec|list|info|stop */
#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgroup.h"
#include "container.h"
#include "lifecycle.h"
#include "rootfs.h"
#include "userns.h"

static void usage(FILE *out)
{
    fputs(
        "BoxForge " BF_VERSION " - minimal educational Linux container runtime\n"
        "\n"
        "Usage:\n"
        "  boxforge run [OPTIONS] <rootfs> <cmd> [args...]\n"
        "  boxforge exec <container> <cmd> [args...]\n"
        "  boxforge list\n"
        "  boxforge info <container>\n"
        "  boxforge stop <container>\n"
        "\n"
        "Options for 'run':\n"
        "  --hostname NAME      container hostname (default: boxforge)\n"
        "  --memory SIZE        cgroup v2 memory.max, e.g. 64M, 256M, 1G\n"
        "  --cpu PERCENT        cgroup v2 cpu.max, percent of one CPU (e.g. 20)\n"
        "  --name NAME          friendly name usable instead of the id\n"
        "  -d, --detach         run in the background, print the container id\n"
        "  --init               run a PID-1 supervisor that reaps zombies and forwards\n"
        "                       signals (the command becomes PID 2); default: the\n"
        "                       command itself is PID 1 (direct exec)\n"
        "  --overlay            use <rootfs> as a read-only lower layer (OverlayFS);\n"
        "                       writes go to a throw-away upper layer\n"
        "  --overlay-dir DIR    like --overlay, but keep upper/ in DIR after exit\n"
        "  --userns             add a user namespace (container uid 0 -> unprivileged host uid)\n"
        "  --userns-uid UID     host uid for container root (default: $SUDO_UID or 65534)\n"
        "  --net none|bridge    none: isolated netns with loopback (default);\n"
        "                       bridge: veth + bridge + NAT (needs iproute2/iptables)\n"
        "  --stop-timeout SEC   grace period before SIGKILL on stop (default 10)\n"
        "  --log FILE           with --detach: write container output to FILE\n"
        "  --chroot             DEBUG: use chroot() instead of pivot_root()\n"
        "  --keep-caps          do not drop dangerous capabilities (less safe)\n"
        "  --no-cgroup          DEBUG: skip cgroup creation (no resource limits)\n"
        "  -v, --verbose        more logging (repeat for debug)\n"
        "  --help, --version\n"
        "\n"
        "Requires root (CAP_SYS_ADMIN). Example:\n"
        "  sudo ./boxforge run --hostname demo --memory 128M --cpu 20 ./rootfs /bin/sh\n",
        out);
}

static bool valid_label(const char *s, size_t max, bool allow_dot)
{
    size_t n = strlen(s), i;

    if (n == 0 || n >= max)
        return false;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) || ch == '-' || ch == '_' || (allow_dot && ch == '.')))
            return false;
    }
    return true;
}

/* "--opt value" or "--opt=value". Returns value or NULL (after an error). */
static const char *opt_value(int argc, char **argv, int *i, const char *name, const char *eq)
{
    if (eq != NULL)
        return eq;
    if (*i + 1 >= argc) {
        bf_err("option %s requires a value", name);
        return NULL;
    }
    return argv[++*i];
}

static int parse_run(int argc, char **argv, struct container *c)
{
    int i;

    for (i = 2; i < argc; i++) {
        char *a = argv[i];
        const char *eq = NULL, *v;
        char name[32];

        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0')
            break;
        if (strncmp(a, "--", 2) == 0 && (eq = strchr(a, '=')) != NULL) {
            snprintf(name, sizeof name, "%.*s", (int)(eq - a), a);
            eq++;
        } else {
            snprintf(name, sizeof name, "%s", a);
        }

        if (!strcmp(name, "--hostname")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            if (!valid_label(v, BF_HOST_MAX, true)) {
                bf_err("invalid hostname '%s'", v);
                return -1;
            }
            snprintf(c->hostname, sizeof c->hostname, "%s", v);
        } else if (!strcmp(name, "--memory")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)) || bf_parse_memory(v, &c->mem_limit) < 0)
                return -1;
        } else if (!strcmp(name, "--cpu")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)) ||
                bf_parse_cpu_percent(v, &c->cpu_percent) < 0)
                return -1;
        } else if (!strcmp(name, "--name")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            if (!valid_label(v, BF_NAME_MAX, true)) {
                bf_err("invalid container name '%s' (use letters, digits, - _ .)", v);
                return -1;
            }
            snprintf(c->name, sizeof c->name, "%s", v);
        } else if (!strcmp(name, "-d") || !strcmp(name, "--detach")) {
            c->opt_detach = true;
        } else if (!strcmp(name, "--init")) {
            c->opt_init = true;
        } else if (!strcmp(name, "--overlay")) {
            c->opt_overlay = true;
        } else if (!strcmp(name, "--overlay-dir")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            if (v[0] != '/') {
                char cwd[PATH_MAX];
                if (!getcwd(cwd, sizeof cwd) ||
                    bf_snprintf(c->overlay_dir, sizeof c->overlay_dir, "%s/%s", cwd, v) < 0) {
                    bf_err("overlay dir path too long");
                    return -1;
                }
            } else if (bf_snprintf(c->overlay_dir, sizeof c->overlay_dir, "%s", v) < 0) {
                return -1;
            }
            c->opt_overlay = true;
        } else if (!strcmp(name, "--userns")) {
            c->opt_userns = true;
        } else if (!strcmp(name, "--userns-uid")) {
            char *end;
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            c->userns_uid = (uid_t)strtoul(v, &end, 10);
            if (*end != '\0' || c->userns_uid == 0) {
                bf_err("--userns-uid must be a non-zero numeric uid");
                return -1;
            }
            c->opt_userns = true;
        } else if (!strcmp(name, "--net")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            if (!strcmp(v, "bridge"))
                c->opt_net_bridge = true;
            else if (strcmp(v, "none") != 0) {
                bf_err("--net must be 'none' or 'bridge'");
                return -1;
            }
        } else if (!strcmp(name, "--stop-timeout")) {
            char *end;
            long t;
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            t = strtol(v, &end, 10);
            if (*end != '\0' || t < 0 || t > 3600) {
                bf_err("--stop-timeout must be 0..3600 seconds");
                return -1;
            }
            c->stop_timeout = (int)t;
        } else if (!strcmp(name, "--log")) {
            if (!(v = opt_value(argc, argv, &i, name, eq)))
                return -1;
            if (v[0] != '/') {
                char cwd[PATH_MAX];
                if (!getcwd(cwd, sizeof cwd) ||
                    bf_snprintf(c->log_file, sizeof c->log_file, "%s/%s", cwd, v) < 0)
                    return -1;
            } else if (bf_snprintf(c->log_file, sizeof c->log_file, "%s", v) < 0) {
                return -1;
            }
        } else if (!strcmp(name, "--chroot")) {
            c->opt_chroot = true;
        } else if (!strcmp(name, "--keep-caps")) {
            c->opt_keep_caps = true;
        } else if (!strcmp(name, "--no-cgroup")) {
            c->opt_no_cgroup = true;
        } else if (!strcmp(name, "-v") || !strcmp(name, "--verbose")) {
            bf_log_set_level(bf_log_get_level() + 1 > BF_LOG_DEBUG ? BF_LOG_DEBUG
                                                                   : bf_log_get_level() + 1);
        } else if (!strcmp(name, "-vv")) {
            bf_log_set_level(BF_LOG_DEBUG);
        } else if (!strcmp(name, "--help") || !strcmp(name, "-h")) {
            usage(stdout);
            exit(0);
        } else {
            bf_err("unknown option '%s' (see boxforge --help)", a);
            return -1;
        }
    }

    if (argc - i < 2) {
        bf_err("usage: boxforge run [OPTIONS] <rootfs> <cmd> [args...]");
        return -1;
    }
    if (argc - i - 1 > BF_MAX_ARGS) {
        bf_err("too many command arguments (max %d)", BF_MAX_ARGS);
        return -1;
    }
    return i;
}

static int cmd_run(int argc, char **argv)
{
    static struct container c; /* large; static avoids a big stack frame */
    int i, k;

    bf_container_init(&c);
    c.userns_uid = (uid_t)-1;
    c.userns_gid = (gid_t)-1;
    i = parse_run(argc, argv, &c);
    if (i < 0)
        return 2;

    for (k = i + 1; k < argc; k++)
        c.argv[c.argc++] = argv[k];
    c.argv[c.argc] = NULL;
    bf_container_set_cmdline(&c);

    if (geteuid() != 0) {
        bf_err("must run as root (CAP_SYS_ADMIN is required to create namespaces and mounts): "
               "try `sudo ./boxforge run ...`");
        return 1;
    }
    if (bf_rootfs_resolve(&c, argv[i]) < 0)
        return 1;
    if (c.opt_userns) {
        if (!bf_userns_supported()) {
            bf_err("this kernel has no user namespace support");
            return 1;
        }
        bf_userns_choose_ids(&c);
    }
    if (bf_runtime_root_ensure() < 0)
        return 1;
    if (c.name[0] && bf_container_name_in_use(c.name)) {
        bf_err("a container named '%s' already exists", c.name);
        return 1;
    }
    bf_container_gen_id(&c);
    signal(SIGPIPE, SIG_IGN);
    return bf_lifecycle_run(&c);
}

int bf_cli_main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h") || !strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
        printf("boxforge %s\n", BF_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "run"))
        return cmd_run(argc, argv);

    if (geteuid() != 0) {
        bf_err("'%s' must run as root (runtime state in %s is root-owned)", cmd,
               bf_runtime_root());
        return 1;
    }
    if (!strcmp(cmd, "list"))
        return bf_lifecycle_list();
    if (!strcmp(cmd, "info") && argc == 3)
        return bf_lifecycle_info(argv[2]);
    if (!strcmp(cmd, "stop") && argc == 3)
        return bf_lifecycle_stop(argv[2]);
    if (!strcmp(cmd, "exec") && argc >= 4)
        return bf_lifecycle_exec(argv[2], argc - 3, argv + 3);

    bf_err("unknown or incomplete command '%s'", cmd);
    usage(stderr);
    return 2;
}
