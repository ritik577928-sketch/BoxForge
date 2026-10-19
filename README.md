# BoxForge — a minimal educational Linux container runtime (C11)

BoxForge is a small Docker-like runtime written in C11 that talks to the Linux
kernel directly: `clone()`, namespaces, `pivot_root()`, cgroups v2, `setns()`,
OverlayFS. **No Docker, runc, containerd or systemd-nspawn is used.** It exists
to make container internals understandable — it is *not* production security software
(see [SECURITY.md](SECURITY.md)).

## Quick start

```bash
./scripts/preflight.sh                 # check kernel features, cgroup v2, tools
make                                   # build ./boxforge
sudo ./scripts/create-rootfs.sh        # static BusyBox rootfs in ./rootfs
sudo ./boxforge run ./rootfs /bin/sh   # PID 1 shell in a fresh container
```

Inside the container try `ps`, `hostname`, `ls /`, `cat /proc/1/status`, `echo $$`.

```bash
make test                              # full automated suite (uses sudo)
sudo ./scripts/audit-host.sh           # prove nothing was leaked
```

## CLI

```text
boxforge run [OPTIONS] <rootfs> <cmd> [args...]
boxforge exec <container> <cmd> [args...]
boxforge list | info <container> | stop <container>

--hostname NAME   --memory 256M   --cpu 20      --name NAME    -d/--detach
--init            --overlay       --overlay-dir DIR
--userns [--userns-uid UID]       --net none|bridge
--stop-timeout S  --log FILE      --chroot (debug)  --keep-caps  --no-cgroup (debug)  -v
```

`<container>` is the id printed by `--detach`, a unique id prefix (>=3 chars) or `--name`.
Runtime metadata lives in `/var/run/boxforge/<id>/` (root-owned, mode 0700;
override for experiments with `BOXFORGE_RUNTIME_DIR=/abs/path`).

## Architecture

```
 boxforge run rootfs cmd
        |
   BoxForge parent (monitor)                      container (child)
   ------------------------                      -----------------
   validate, make id + runtime dir
   block SIGINT/TERM/HUP/CHLD
   create cgroup, set memory.max/cpu.max
   [overlay dirs] [bridge/NAT prep]
   clone(NEWPID|NEWNS|NEWUTS|NEWIPC|NEWNET) ---> starts as PID 1, blocks on sync pipe
   [write uid_map/gid_map]
   move child pid into cgroup
   [create veth, move peer to child netns]
   write 'G' on sync pipe -------------------->  GO
   read error pipe (EOF == exec succeeded)        sethostname, lo up, [eth0 config]
   state=running                                  mount --make-rprivate /
   monitor loop (sigtimedwait + waitpid)          [mount overlay]
                                                  bind rootfs, /dev tmpfs, /proc
                                                  pivot_root, umount old root
                                                  drop capabilities, clean env
                                                  exec cmd   (or fork+supervise with --init)
   child exits -> read memory.events (OOM?)
   cleanup: veth, cgroup, overlay, metadata
```

Module map: `clone_runtime.c` (clone + child side), `namespaces.c`, `rootfs.c`, `cgroup.c`,
`signals.c`, `lifecycle.c` (parent orchestration, stop/exec/list/info), `container.c`
(descriptor, metadata, rollback), `overlay.c`, `userns.c`, `network.c`, `cli.c`, `util.c`.
There is no global mutable state except the log level / last error string in `util.c`.

### Container vs VM
A container is an ordinary process (tree) of the **host kernel**, given private *views*
(namespaces) and *limits* (cgroups). There is no guest kernel and no hardware
virtualisation: `uname -r` inside is the host's. A VM runs its own kernel on emulated
hardware. Consequence: a kernel bug or an over-privileged container is a host problem.

### Namespaces
| Namespace | Flag | What is isolated |
|---|---|---|
| PID | `CLONE_NEWPID` | process IDs; first process is PID 1, host processes invisible |
| Mount | `CLONE_NEWNS` | the mount table; we `MS_REC\|MS_PRIVATE` first so nothing propagates to the host |
| UTS | `CLONE_NEWUTS` | hostname / domainname |
| IPC | `CLONE_NEWIPC` | SysV shm/sem/msg queues, POSIX mqueues |
| Network | `CLONE_NEWNET` | interfaces, routes, ports. A new netns has only a *down* `lo` (BoxForge brings it up) — **no Internet** |
| User (optional) | `CLONE_NEWUSER` | uid/gid mapping: container root = unprivileged host uid |

Host vs container PID: the container's PID 1 is e.g. host PID 1672; `boxforge info` shows both.

### Cgroups v2
BoxForge creates `<cgroup2 mount>/boxforge/<id>/` and writes
`memory.max` (bytes; `memory.swap.max=0` and `memory.oom.group=1` make the limit real and
kill the whole group), and `cpu.max` as `"<quota> <period>"` with period 100000 µs:
`--cpu 20` → `20000 100000` (20 % of one CPU). Only the unified v2 hierarchy is used.
The child is moved into the cgroup **before** it is told to continue (sync pipe),
so limits are active before the application runs. On exit BoxForge reads `memory.events`
(`oom_kill`) and reports OOM kills, then removes the cgroup (using `cgroup.kill` if needed).

### Rootfs, /proc, /dev, pivot_root
1. rootfs is `realpath`-resolved and checked; 2. all mounts made private;
3. rootfs bind-mounted onto itself (pivot_root needs a mount point);
4. `/dev` = fresh tmpfs with only `null zero full random urandom tty` bind-mounted from the host,
   `/dev/shm`, `/dev/fd`, `stdin/out/err`; 5. `/proc` mounted from inside the PID namespace
   (so `ps` shows the container view), with `/proc/sys` etc. read-only and `kcore` masked;
6. `pivot_root(root, root/.pivot_old)`, `chdir /`, `umount2(MNT_DETACH)`, `rmdir`.
(If the rootfs is not writable — user namespace case — the stacked `pivot_root(".", ".")` form is used.)
`chroot` is available only as `--chroot` debug mode: it does not change the mount table, so it
is *not* isolation.

### PID 1 and zombies — the design decision
`clone(CLONE_NEWPID)` makes the child PID 1. Two requirements collide: "the command is PID 1"
and "PID 1 reaps orphans". After `exec`, the **application** owns PID 1 and an arbitrary program does
not reap orphans or handle SIGTERM as init. BoxForge therefore offers both honestly:

| Mode | PID 1 is | Command is | Reaps orphans | Use |
|---|---|---|---|---|
| default (direct exec) | the command | PID 1 | only if the command does | demos, `echo $$` = 1 |
| `--init` | BoxForge supervisor | PID 2 | **yes** (`waitpid(-1)`), forwards INT/TERM/HUP/USR1/USR2/QUIT, exits with the command's status, SIGTERM→SIGKILL sweep of leftovers | robust workloads |

`tests/fork_tree orphan` shows it: `--init` → `zombies=0`; direct mode → `zombies=1`.
Note the kernel only delivers signals from the host to a namespace's PID 1 if it installed a handler
(SIGKILL/SIGSTOP excepted), so `boxforge stop` on a direct-mode program that ignores SIGTERM waits
`--stop-timeout` seconds and then escalates to SIGKILL.

### Lifecycle
`create` (descriptor, id, runtime dir) → `setup` (cgroup, overlay, net, clone, maps) → `exec`
(GO byte, error pipe EOF) → `monitor` (SIGCHLD/waitpid; INT/TERM/HUP → SIGTERM → wait → SIGKILL)
→ `exit` (status: `N`, or `128+signal`; 125 setup failure, 126 not executable, 127 not found)
→ `cleanup` (rollback structure `bf_cleanup`: veth, cgroup, overlay, runtime dir; mounts vanish with
the mount namespace). Cleanup runs on normal exit, failure, `execvp` failure, signals and
half-finished setup.

A Ctrl-C typed in the terminal goes to the whole foreground process group, so the container gets
it directly; BoxForge only starts a stop for signals that did **not** originate from the tty.

### `boxforge exec` and setns()
Opens `/proc/<pid>/ns/{user,mnt,uts,ipc,net,pid}` of the container's init **first** (the host `/proc`
is unreachable after entering the mount namespace), then `setns()` in the order user → mnt → uts →
ipc → net → pid, after moving itself into the container's cgroup. `setns(CLONE_NEWPID)` does *not* change
the caller's own PID; only **children** created afterwards live in the namespace, so BoxForge forks a
helper that execs the command (it gets e.g. PID 3). The helper drops the same capabilities.

### Overlay
`--overlay`: `lower` = your rootfs (read-only in effect), `upper` and `work` created under the
runtime dir (deleted at exit), `merged` = the container root (mounted inside the container's mount
namespace, so it never appears on the host). `--overlay-dir DIR` keeps `DIR/upper` afterwards.
Test: write `/tmp/x` in the container → absent in `rootfs/tmp`, present in `DIR/upper/tmp/x`.

### User namespace (optional)
`--userns [--userns-uid UID]`: the parent writes `/proc/<pid>/setgroups` (`deny`), `gid_map`, `uid_map` =
`0 <hostuid> 1`; the child then `setresuid(0,0,0)`. Inside `id -u` = 0, but on the host the process runs as
`<hostuid>` (default `$SUDO_UID`, else 65534) — verify with `grep Uid /proc/<hostpid>/status`.

### Networking
* **CORE (`--net none`, default):** private netns with only `lo` (up). No connectivity — by design.
* **STRETCH (`--net bridge`):** bridge `bfbr0` (10.88.0.1/24) + veth pair per container
  (`bfh<id>`/`bfc<id>`), peer moved into the container netns and configured (ioctl) as `eth0` with a
  unique 10.88.0.x address and default route; IPv4 forwarding + `iptables` MASQUERADE/FORWARD rules
  are added idempotently and removed with the last container; pre-existing networking is not touched.
  Needs `ip` (iproute2) and `iptables`. Test: `sudo ./tests/test_network.sh`.

## Build / run / test

```bash
sudo apt update && sudo apt install -y build-essential busybox-static iproute2 iptables
make                       # gcc -Wall -Wextra -O2 -std=c11, zero warnings
sudo ./scripts/create-rootfs.sh            # BusyBox (installed, found, or downloaded)
sudo ./scripts/create-rootfs.sh --host-tools   # offline fallback (dynamic host binaries)
sudo ./boxforge run --hostname demo --memory 128M --cpu 20 ./rootfs /bin/sh
sudo ./boxforge run ./rootfs /bin/sh -c "echo hello"
sudo ./boxforge run --init ./rootfs /bin/sh          # supervisor mode
make test                  # tests/run_tests.sh (15 numbered tests + exec/userns/audit)
sudo ./tests/test_network.sh                # stretch networking
sudo ./scripts/cleanup.sh [--all]           # remove leftovers from crashes
```

### Demo script (Week-12 style)
```bash
sudo ./boxforge run --hostname demo --memory 64M --cpu 20 --name demo ./rootfs /bin/sh
#  echo $$   -> 1          (host PID differs: see `boxforge info` from another terminal)
#  hostname  -> demo       (host: `hostname` unchanged)
#  ls /; ps; cat /proc/1/status; cat /proc/net/dev   (private fs, /proc, netns with only lo)
sudo ./boxforge run --memory 32M ./rootfs /opt/bf-tests/memhog 4 512   # OOM-killed, exit 137
sudo ./boxforge run --cpu 20 ./rootfs /opt/bf-tests/cpuhog 4           # cpu_share≈20%
ID=$(sudo ./boxforge run -d --init ./rootfs /bin/sleep 300)
sudo ./boxforge exec $ID /bin/sh -c 'echo $$; hostname'                # setns demo
sudo ./boxforge stop $ID; sudo ./scripts/audit-host.sh                 # no leaks
sudo ./boxforge run --overlay-dir /tmp/ov ./rootfs /opt/bf-tests/file_write_test /tmp/f hi
ls rootfs/tmp/f /tmp/ov/upper/tmp/f                                    # lower clean, upper has it
sudo ./boxforge run --userns --userns-uid 1000 ./rootfs /bin/id        # uid=0 inside
sudo ./boxforge run -d --init --net bridge ./rootfs /bin/sleep 60      # veth/NAT (needs ip, iptables)
```
(Test helpers are copied into `rootfs/opt/bf-tests` by `make test`; run it once first.)

## Troubleshooting
| Symptom | Fix |
|---|---|
| `must run as root` | use `sudo` (CAP_SYS_ADMIN is required to create namespaces/mounts) |
| `cgroup v2 is not mounted` | boot with `systemd.unified_cgroup_hierarchy=1` or `sudo mount -t cgroup2 none /sys/fs/cgroup`; `--no-cgroup` skips limits |
| `'memory'/'cpu' controller is not available` | host is v1/hybrid: the controllers are bound to v1 hierarchies. Use a pure v2 host. **WSL2**: add `[wsl2]` `kernelCommandLine = cgroup_no_v1=all` to `%UserProfile%\.wslconfig`, then `wsl --shutdown` |
| `pivot_root: Invalid argument` | the host `/` is an initramfs/rootfs mount; use `--chroot` (debug) |
| `no static BusyBox` | `sudo apt install busybox-static` or `create-rootfs.sh --host-tools` |
| `mount overlay ... Invalid argument` | upper/work must be on a filesystem supporting overlay (not another overlay/NFS); try `--overlay-dir` on ext4 |
| `--userns` fails on Ubuntu 24.04 | AppArmor may restrict user namespaces: `sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0` (only matters for unprivileged use; BoxForge runs as root) |
| leftovers after a crash | `sudo ./scripts/audit-host.sh`, then `sudo ./scripts/cleanup.sh` |
| `command not found inside rootfs` | the rootfs lacks that binary (or an absolute symlink points outside it) |
