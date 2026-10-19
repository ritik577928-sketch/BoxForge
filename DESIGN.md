# BoxForge Design

## Requirement → Mechanism → Module → Test

| Requirement | Linux mechanism | Module | Acceptance Test |
|---|---|---|---|
| Process creation | `clone()` with mmap'd stack + guard page; stack freed after clone | clone_runtime.c | A1, A2 |
| PID isolation | PID namespace (`CLONE_NEWPID`) | namespaces.c / clone_runtime.c | A4, A5 |
| Filesystem isolation | mount namespace + `MS_REC\|MS_PRIVATE` + bind + `pivot_root` | rootfs.c | A7, A8 |
| `/proc` | `mount -t proc` inside the PID ns (container view) + harden | rootfs.c | A9 |
| `/dev` | private tmpfs + bind-mounted device nodes + shm + symlinks | rootfs.c | A7 |
| Hostname isolation | UTS namespace + `sethostname` | namespaces.c | A6 |
| IPC isolation | IPC namespace (`CLONE_NEWIPC`) | namespaces.c | A10 |
| Network isolation (core) | network namespace + loopback up | namespaces.c | A11 |
| Real networking (stretch) | veth + bridge + NAT (ip, iptables, ioctl) | network.c | A20 |
| CPU control | cgroup v2 `cpu.max` | cgroup.c | A13 (`cpuhog`) |
| Memory control | cgroup v2 `memory.max`, `memory.swap.max`, `memory.oom.group` | cgroup.c | A12 (`memhog`) |
| No start-before-limit race | sync pipe: cgroup attach before `GO`; child blocks reading pipe | lifecycle.c / clone_runtime.c | A12, A13 |
| PID 1 / zombie reaping | direct exec (cmd=PID1) or `--init` supervisor with `waitpid(-1)` | clone_runtime.c | A5 (`fork_tree`) |
| Signal handling / stop | signal mask + `sigtimedwait`; SIGTERM → wait → SIGKILL escalation | signals.c, lifecycle.c | A14 (`signal_test`) |
| Clean teardown | `bf_cleanup` flags, rollback; mounts disappear with the mount ns | container.c | A15 + `audit-host.sh` |
| Running-container entry | `setns()` in correct order + helper fork for PID ns | namespaces.c, lifecycle.c | A18 |
| Overlay root | OverlayFS lower/upper/work/merged; transparent to container | overlay.c | A17 (`file_write_test`) |
| User namespace | `CLONE_NEWUSER`, uid/gid_map, `setgroups deny` | userns.c | A19 |
| Reduced privilege | `PR_CAPBSET_DROP` of dangerous caps | namespaces.c | capability check (manual) |
| Exit status | `WIFEXITED`/`WIFSIGNALED` propagated; 125=setup, 126=noexec, 127=notfound | lifecycle.c | A3 |

---

## Exact Runtime Sequence

### Parent Side (`bf_lifecycle_run`)

```
parse/validate CLI
    │
    ▼
require root (geteuid == 0)
    │
    ▼
validate rootfs  ← realpath, is-dir, command exists inside
    │
    ▼
[--detach]: fork, setsid, redirect stdio, launcher waits on status pipe
    │
    ▼
generate container id  ← getrandom → hex
create runtime dir     ← /var/run/boxforge/<id>/ (mode 0700)
    │
    ▼
block signals          ← SIGINT/SIGTERM/SIGHUP/SIGCHLD → sigprocmask
save state=created
    │
    ▼
cgroup                 ← detect cgroup2 mount
                          check controllers available
                          mkdir boxforge/<id>
                          write memory.max (if --memory)
                          write cpu.max    (if --cpu)
                          write memory.swap.max=0, memory.oom.group=1
    │
    ▼
[--overlay]:           create upper/work/merged directories
    │
    ▼
[--net bridge]:        lock, ensure bridge+NAT, pick container IP, save
    │
    ▼
pipe2(sync_pipe)       ← parent→child: 'G'=go, 'X'=abort
pipe2(err_pipe)        ← child→parent: setup error text or EOF on success
    │
    ▼
clone(child_main, stack_top, CLONE_NEWPID|NEWNS|NEWUTS|NEWIPC|NEWNET[|NEWUSER]|SIGCHLD)
    │                                                                    ↓
    │                                                             CHILD WAITS
    │                                                             (read sync_pipe[0])
    ▼
[--userns]: write /proc/<pid>/setgroups=deny
            write /proc/<pid>/gid_map  (0 → host_gid)
            write /proc/<pid>/uid_map  (0 → host_uid)
    │
    ▼
cgroup_add_pid         ← write <pid> to cgroup.procs
    │
    ▼
[--net bridge]: ip link add veth_host type veth peer veth_peer
                ip link set veth_peer netns <pid>
                ip link set veth_host master bfbr0 up
    │
    ▼
write 'G' to sync_pipe[1]   ← CHILD UNBLOCKS
close(sync_pipe[1])
    │
    ▼
read err_pipe[0]            ← EOF = exec succeeded; text = failure
    │
    ├── failure → kill child, wait, cleanup, return BF_EXIT_SETUP
    │
    ▼
state=running
report_ready (detach mode: write "OK <id>" to launcher pipe)
    │
    ▼
monitor loop:
    sigtimedwait(SIGINT|SIGTERM|SIGHUP|SIGCHLD, 200ms)
    waitpid(WNOHANG)
    if stopping && elapsed >= stop_timeout: SIGKILL (+ cgroup.kill)
    │
    ▼
child exited
read memory.events (oom_kill)
state=exited
    │
    ▼
cleanup:
    bf_net_teardown    ← veth del; bridge+NAT only if we created them and no members left
    bf_cgroup_remove   ← rmdir with retry; try parent cgroup when empty
    bf_overlay_cleanup ← umount merged; rm work; rm upper (unless --overlay-dir)
    rm -rf runtime_dir
    │
    ▼
return exit_code
```

### Child Side (`child_main` — PID 1 of new namespace)

```
close(sync_pipe[1], err_pipe[0])   ← close parent ends
close(ready_fd)                    ← not our pipe
prctl(PR_SET_PDEATHSIG, SIGKILL)   ← die if parent dies
restore signal mask                ← sigprocmask(SIG_SETMASK, saved_mask)
    │
    ▼
read sync_pipe[0]                  ← BLOCK until parent writes 'G'
close(sync_pipe[0])
if != 'G': _exit(BF_EXIT_SETUP)
    │
    ▼
[--userns]: setresgid(0,0,0), setresuid(0,0,0)   ← become uid/gid 0 inside ns
    │
    ▼
sethostname(hostname)              ← UTS namespace
    │
    ▼
loopback up                        ← SIOCGIFFLAGS/SIOCSIFFLAGS on "lo"
    │
    ▼
[--net bridge]: rename veth_peer→eth0, assign IP, netmask, up, add default route
    │
    ▼
mount(NULL, "/", MS_REC|MS_PRIVATE)   ← CRITICAL: private propagation FIRST
    │
    ▼
[--overlay]: mount overlay on merged (lowerdir=rootfs, upperdir, workdir)
             root = merged
    │
    ▼
bind-mount root onto itself        ← pivot_root requires new_root to be a mountpoint
    │
    ▼
setup_dev(root):
    mkdir root/dev
    mount tmpfs on root/dev (MS_NOSUID|MS_STRICTATIME, mode=755,size=64k)
    bind-mount /dev/null, /dev/zero, /dev/full, /dev/random, /dev/urandom, /dev/tty
    mkdir root/dev/shm → tmpfs
    symlinks: fd→/proc/self/fd, stdin/stdout/stderr
    │
    ▼
mount_proc(root):
    mkdir root/proc
    mount -t proc (MS_NOSUID|MS_NODEV|MS_NOEXEC)
    harden: bind /proc/sys readonly; mask kcore, keys, timer_list
    │
    ▼
pivot_root(root, root/.pivot_old):
    mkdir root/.pivot_old
    syscall(SYS_pivot_root, root, root/.pivot_old)
    chdir("/")
    umount2("/.pivot_old", MNT_DETACH)
    rmdir("/.pivot_old")

    [fallback if rootfs unwritable]: pivot_root(".", ".") + umount2(".", MNT_DETACH)
    │
    ▼
chdir("/")
    │
    ▼
[!--keep-caps]: PR_CAPBSET_DROP dangerous capabilities
    │
    ▼
clearenv(), set PATH/HOME/HOSTNAME/TERM/container
    │
    ▼
[direct mode]: exec_app()
    execvp(argv[0], argv)   ← TARGET IS PID 1; signal(SIGPIPE, SIG_DFL) first
    on failure: write "<code> <what>: <strerror>" to err_pipe[1], _exit(code)

[--init mode]: run_init()   ← see below; NEVER returns
```

---

## PID-1 Design: Direct Mode vs Init Mode

This is an explicit design choice that must be understood. Two valid concepts exist:

### Direct Mode (default)

```
BoxForge parent (monitor)         Container namespace
         |                              |
      clone() ─────────────────────► child_main() [PID 1]
                                        |
                                     setup: hostname, mounts, /proc, /dev,
                                            pivot_root, drop caps, env
                                        |
                                     execvp(target)    ← PID 1 IS NOW TARGET
                                        |
                                     [target runs as PID 1]
```

**Consequence**: The target application owns PID 1. In Linux, PID 1 has special signal semantics — signals only arrive if the process installed a handler. An arbitrary application does NOT automatically reap orphans. If the application forks and the child exits, the grandchild becomes an orphan adopted by PID 1 (the application), which may or may not reap it.

**When to use**: Demos, `echo $$` = 1, simple single-process workloads, shells.

### Init Mode (`--init`)

```
BoxForge parent (monitor)         Container namespace
         |                              |
      clone() ─────────────────────► child_main() [PID 1 = BoxForge supervisor]
                                        |
                                     setup...
                                        |
                                     fork() ──────────────► target [PID 2]
                                        |                       |
                                        |                    execvp(target)
                                        |
                                     sigwaitinfo(SIGCHLD | fwd_signals)
                                        │
                                        ├─ SIGCHLD → waitpid(-1, WNOHANG)
                                        │             if target exited: done=true, record code
                                        │
                                        ├─ SIGTERM/INT/HUP/USR1/USR2/QUIT → kill(target, sig)
                                        │
                                        └─ target done: kill(-1, SIGTERM); reap_all(2000ms)
                                                        kill(-1, SIGKILL); reap_all(500ms)
                                                        _exit(target_code)
```

**Properties**:
- PID 1 = BoxForge supervisor (reaps ALL orphans)
- Target = PID 2+ (does NOT need to be init-aware)
- All SIGTERM/INT/HUP/USR1/USR2/QUIT forwarded to target
- Graceful teardown: SIGTERM→wait→SIGKILL sweep of any survivors
- Target's exit code propagated

**When to use**: Any workload that may fork, long-running services, robust production-like use.

**Key rule**: Do NOT claim that `exec()` makes an arbitrary application a proper init. Only `--init` mode guarantees orphan reaping.

---

## Cgroup v2 Synchronization (No Race Condition)

The child is placed into its cgroup **before** it is allowed to run any application code:

```
Parent:                              Child (PID 1 in namespace):
  cgroup_create()                      read(sync_pipe) ──── BLOCKS HERE
  cgroup_add_pid(child_pid)
  write(sync_pipe, "G")  ─────────────► unblocks, proceeds
  [limits active before exec]
```

`memory.max` and `cpu.max` are written before `G` is sent. The child cannot exec the application until it receives `G`. Therefore, **no application code runs before cgroup limits are active**.

---

## Stop / Signal Policy

| Signal source | BoxForge action |
|---|---|
| SIGTERM / SIGHUP from outside terminal | Forward SIGTERM to container PID 1; wait `--stop-timeout`; escalate SIGKILL |
| SIGINT from TTY (si_code == SI_KERNEL) | Do NOT stop — already reached the container process group |
| SIGINT from non-TTY source | Forward SIGTERM (same as SIGTERM) |
| `boxforge stop <id>` | Signal monitor with SIGTERM → monitor runs graceful stop → cleanup |
| Container process exits normally | Monitor reaps it; cleanup; report exit code |

Repeated SIGTERM while stopping: ignored (no restart of stop timer).

SIGKILL escalation: after `--stop-timeout` seconds, also writes `cgroup.kill` (kernel >= 5.14) to kill any leaked processes in the cgroup.

---

## Network Ownership Rules

BoxForge tracks all network resources it creates:

| Resource | Created by BoxForge? | Marker |
|---|---|---|
| Bridge `bfbr0` | Only if not present before | `$RT/bridge.created` |
| IPv4 forwarding | Only if was `0` before | `$RT/ip_forward.prev` |
| iptables NAT/FORWARD rules | Only if BoxForge created bridge | (no marker; deleted with bridge) |
| veth pair `bfh<id>` / `bfc<id>` | Yes, per container | container state file |

**Critical rule**: BoxForge never deletes the bridge or firewall rules if it did NOT create them (no `bridge.created` marker). Pre-existing host networking is never modified.

Partial network init failure rolls back completely (veth is deleted if bridge attach fails).

---

## Why These Design Choices

| Choice | Reason |
|---|---|
| Error pipe with CLOEXEC | Exact exec/setup errors; EOF = exec succeeded; no polling |
| Stack freed after clone | No CLONE_VM; child has own copy; parent can free immediately |
| Mounts need no host cleanup | All created inside container's private mount namespace; disappear with it |
| `pivot_root` not `chroot` | Old root is detached; host tree unreachable; true isolation |
| Sync pipe before exec | Guarantees cgroup limits are active before any app code runs |
| flock for network | Prevents concurrent veth/bridge races with multiple `boxforge run --net bridge` |
| Stacked `pivot_root(".",".") ` | Handles read-only rootfs in user namespace mode |
| `FTW_MOUNT` in rm_rf | Prevents accidentally crossing filesystem boundaries during cleanup |
| `PR_SET_PDEATHSIG=SIGKILL` | Child is killed immediately if parent dies unexpectedly |
