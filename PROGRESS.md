# BoxForge — Project Progress Checkpoint

Last updated: 12 October 2026
Project: BoxForge — Minimal Linux Container Runtime
Location: /home/ritik/boxforge/boxforge
Environment: Ubuntu on WSL2
Kernel: 6.18.40.1-microsoft-standard-WSL2+

## 1. Latest verified test results

### Acceptance tests
Command:
sudo bash ./tests/acceptance.sh

Result:
ACCEPTANCE RESULT: 49 passed, 0 failed, 0 skipped

### Functional tests
Command:
sudo bash ./tests/run_tests.sh

Result:
RESULT: 55 passed, 0 failed, 0 skipped

### Host resource audit
Command:
sudo bash ./scripts/audit-host.sh

Result:
audit: CLEAN — no BoxForge resource leaks found

## 2. Verified project functionality

- PID namespace isolation and container PID 1.
- UTS hostname isolation.
- Mount namespace and private root filesystem.
- IPC namespace isolation.
- Network namespace isolation.
- cgroup v2 memory.max enforcement.
- cgroup v2 cpu.max enforcement.
- Signal handling and exit-status propagation.
- Init supervisor and orphan reaping.
- Overlay filesystem preserving the base root filesystem.
- boxforge exec, list, info, and stop.
- User namespace mapping: container UID 0 to host UID 1000.
- Bridge networking with eth0 inside the container and bfbr0 on the host.
- Ten repeated start/stop cycles with successful cleanup.

## 3. Networking fix

Bridge networking initially failed because iptables could not use
MASQUERADE and conntrack extensions.

The following modules were loaded successfully:
- nft_compat
- xt_MASQUERADE
- xt_conntrack

Persistent startup configuration:
 /etc/modules-load.d/boxforge.conf

Contents:
 nft_compat
 xt_MASQUERADE
 xt_conntrack

The systemd-modules-load.service restart succeeded, and all three
modules were visible through lsmod.

IMPORTANT: Automatic module loading after a complete WSL shutdown
and restart has NOT yet been verified.

## 4. Permission fixes

Executable permissions were corrected for several rootfs applets,
test programs, and scripts, including:
- rootfs/bin/busybox
- rootfs/bin/cat
- rootfs/bin/hostname
- rootfs/bin/id
- rootfs/bin/ls
- rootfs/bin/readlink
- rootfs/bin/sleep
- rootfs/opt/bf-tests/*
- scripts/audit-host.sh
- tests/acceptance.sh
- tests/run_tests.sh

The acceptance suite passed after these fixes.

## 5. Important behavior observed

Direct mode can leave a zombie when the target process runs as PID 1.
The --init supervisor successfully reaps orphans. Use --init when
supervisor-based process reaping is required.

## 6. Commands to resume work

Open the project:
cd /home/ritik/boxforge/boxforge

Build:
make

Run acceptance tests:
sudo bash ./tests/acceptance.sh

Run functional tests:
sudo bash ./tests/run_tests.sh

Audit host resources:
sudo bash ./scripts/audit-host.sh

Inspect available commands:
./boxforge --help

## 7. Remaining task

Verify persistence after a full WSL shutdown and restart.

After reopening Ubuntu:
1. Check the three networking modules with lsmod.
2. Check systemd-modules-load.service status.
3. Run sudo bash ./tests/acceptance.sh.
4. Confirm A20 bridge networking passes.
5. Run sudo bash ./scripts/audit-host.sh.

Record the new results here after verification.

## 8. Project integrity

Keep the source code, include directory, Makefile, rootfs, tests,
scripts, README.md, DESIGN.md, SECURITY.md, and LICENSE.

Do not delete the rootfs or test files during ordinary cleanup.
The latest successful test results are recorded above.
