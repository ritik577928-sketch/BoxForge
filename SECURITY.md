# BoxForge security notes

BoxForge is an **educational** runtime. It is not as secure as Docker, Podman, runc or any
hardened runtime and must not be used to contain untrusted code.

## Why root / capabilities
Creating PID/mount/UTS/IPC/network namespaces, `mount`, `pivot_root`, `sethostname`, writing cgroups,
`setns`, and writing `uid_map` for an arbitrary host uid all require **CAP_SYS_ADMIN** (plus
CAP_SYS_CHROOT, CAP_SETUID/GID, CAP_NET_ADMIN for networking, CAP_DAC_OVERRIDE for cgroup files).
BoxForge simply requires root. A bug in BoxForge therefore runs with full host privileges.

## What isolation is provided
* PID, mount, UTS, IPC, network (and optionally user) namespaces; private mount propagation.
* Own `/dev` tmpfs with six device nodes; `/proc/sys`, `sysrq-trigger`, `irq`, `bus` read-only,
  `kcore`/`keys`/`timer_list` masked (best effort).
* Capability bounding set reduced (no SYS_ADMIN, SYS_MODULE, SYS_RAWIO, SYS_PTRACE, SYS_BOOT, SYS_TIME,
  NET_ADMIN, MAC_*, AUDIT_CONTROL, SYSLOG, ...) unless `--keep-caps`.
* cgroup v2 memory/cpu limits.

## What is NOT provided
* **No seccomp filter** — the full syscall surface of the kernel is reachable.
* **No AppArmor/SELinux profile**, no `no_new_privs`, no masked `/sys` (it is simply not mounted).
* **No time, cgroup, or PID-count (`pids.max`) limits, no I/O limits**: a fork bomb is not contained.
* Without `--userns`, container root **is host root** (uid 0) with a reduced capability set: a kernel
  vulnerability, an exposed device, or a writable host path means host compromise.
* Host `/dev/null,zero,full,random,urandom,tty` are bind-mounted (shared with the host).
* The rootfs directory is used in place: processes with write access modify the host files, unless `--overlay`.
* `chroot` (`--chroot`) is not a security boundary at all (root can escape; mounts are shared).
* Network: `--net none` gives no connectivity. `--net bridge` places all containers on one L2 bridge
  (they can talk to each other and to the host's bridge address) and NATs them to the Internet; there is no
  per-container firewalling, and enabling IPv4 forwarding affects the whole host while a container runs.

## User namespace model
With `--userns`, container uid/gid 0 map to a single unprivileged host uid/gid; capabilities apply only
inside the namespace and only to resources the namespace owns. This is much safer, but: only one id is
mapped (host-root-owned files appear as `nobody`), `setgroups` is denied, and user namespaces enlarge the
kernel attack surface (some distributions restrict them). Overlay in a user namespace needs `userxattr`.

## Operational risks
* Runtime state lives in `/var/run/boxforge` (checked: owned by the running user, not group/world-writable).
  `BOXFORGE_RUNTIME_DIR` is honored for tests — do not set it from untrusted environments; do not run
  `boxforge` through sudo rules that preserve arbitrary environment.
* Container command arguments are passed to `execvp` without a shell; ids/names are validated, paths are
  length-checked, overlay paths with `, : \` are rejected.
* Use only rootfs images you built or trust.
