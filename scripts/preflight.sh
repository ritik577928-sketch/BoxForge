#!/bin/bash
# preflight.sh - verify the host can build and run BoxForge.
# Usage: ./scripts/preflight.sh [ROOTFS_DIR]     (run with sudo for the complete check)
# Exit status: 0 = no blocking problems, 1 = at least one [BLOCKED].
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="${1:-$ROOT/rootfs}"
fails=0; warns=0
ok()      { printf "[PASS]    %s\n" "$*"; }
warn()    { printf "[WARNING] %s\n" "$*"; warns=$((warns+1)); }
fail()    { printf "[BLOCKED] %s\n" "$*"; fails=$((fails+1)); }
info()    { printf "[INFO]    %s\n" "$*"; }
fix()     { printf "          fix: %s\n" "$*"; }
skip()    { printf "[SKIP]    %s\n" "$*"; }

echo "=== BoxForge Pre-flight Check ==="
echo

# --- platform ---------------------------------------------------------------
echo "-- Platform --"
if [ "$(uname -s)" = "Linux" ]; then
    ok "Linux kernel $(uname -r)"
else
    fail "not Linux ($(uname -s)) — BoxForge requires Linux"
    exit 1
fi

kver_major=$(uname -r | cut -d. -f1)
kver_minor=$(uname -r | cut -d. -f2 | cut -d- -f1)
if [ "$kver_major" -gt 5 ] || { [ "$kver_major" -eq 5 ] && [ "$kver_minor" -ge 10 ]; }; then
    ok "Kernel version $kver_major.$kver_minor (>= 5.10 required)"
else
    fail "Kernel $kver_major.$kver_minor is too old (need >= 5.10 for full cgroup v2 + overlay support)"
fi

case "$(uname -m)" in
    x86_64|aarch64) ok "architecture $(uname -m)" ;;
    *) warn "architecture $(uname -m) is untested (x86_64/aarch64 expected)" ;;
esac

WSL=0
if grep -qi -E "microsoft|wsl" /proc/version 2>/dev/null; then
    WSL=1; info "WSL detected: $(cut -c1-80 /proc/version)"
    if grep -qi "WSL2" /proc/version 2>/dev/null || uname -r | grep -qi "microsoft-standard"; then
        ok "WSL2 (real Linux kernel): namespaces work, cgroup v2 depends on kernel command line"
    else
        fail "WSL1 has no namespaces/cgroups"
        fix "wsl --set-version <distro> 2   (run in Windows PowerShell)"
    fi
fi
echo

# --- tools ------------------------------------------------------------------
echo "-- Required Tools --"
for t in gcc make mount; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t found ($(command -v "$t"))"
    else
        fail "$t missing"
        fix "sudo apt update && sudo apt install -y build-essential util-linux"
    fi
done

echo
echo "-- Optional Tools --"
for t in unshare ip iptables; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t found"
    else
        case "$t" in
            unshare) warn "unshare missing (namespace creation test will be limited)"
                     fix "sudo apt install util-linux" ;;
            ip)      warn "ip missing — only needed for --net bridge"
                     fix "sudo apt install iproute2" ;;
            iptables) warn "iptables missing — only needed for NAT in --net bridge"
                      fix "sudo apt install iptables" ;;
        esac
    fi
done

# Static libc check
if [ -e /usr/lib/x86_64-linux-gnu/libc.a ] || \
   [ -e /usr/lib/aarch64-linux-gnu/libc.a ] || \
   find /usr/lib -name "libc.a" 2>/dev/null | grep -q .; then
    ok "static libc present (test helpers link statically)"
else
    warn "libc.a missing: 'make testbins' will fail"
    fix "sudo apt install libc6-dev"
fi
echo

# --- privileges -------------------------------------------------------------
echo "-- Privileges --"
IS_ROOT=0
if [ "$(id -u)" -eq 0 ]; then
    IS_ROOT=1; ok "root privileges (uid 0)"
else
    warn "not root: BoxForge needs root (CAP_SYS_ADMIN) to run; some checks below are skipped"
    fix "re-run as: sudo ./scripts/preflight.sh"
fi
echo

# --- namespace support -------------------------------------------------------
echo "-- Namespace Support --"
missing=""
for ns in pid mnt uts ipc net; do
    if [ -e "/proc/self/ns/$ns" ]; then
        ok "/proc/self/ns/$ns present"
    else
        fail "kernel lacks namespace: $ns"
        missing="$missing $ns"
    fi
done

if [ -z "$missing" ]; then
    # Test actual namespace creation capability
    if [ $IS_ROOT -eq 1 ] && command -v unshare >/dev/null 2>&1; then
        if unshare --mount --uts --ipc --net --pid --fork true 2>/dev/null; then
            ok "namespace creation test PASSED (PID, mount, UTS, IPC, network)"
        else
            fail "namespace creation test FAILED (unshare)"
            fix "check that CONFIG_*_NS is set and no seccomp/AppArmor policy blocks it"
        fi
    elif command -v unshare >/dev/null 2>&1; then
        # Non-root: try unprivileged unshare
        if unshare --user true 2>/dev/null; then
            ok "unprivileged unshare works (full test needs root)"
        else
            warn "unshare failed as non-root (expected if not root; re-run with sudo)"
        fi
    else
        skip "namespace creation test (unshare not found)"
    fi
else
    fail "missing namespaces:$missing — BoxForge cannot run"
fi

# User namespace
echo
if [ -e /proc/self/ns/user ]; then
    if command -v unshare >/dev/null 2>&1 && unshare --user true 2>/dev/null; then
        ok "user namespaces available (optional --userns)"
    elif [ $IS_ROOT -eq 1 ] && command -v unshare >/dev/null 2>&1; then
        if unshare --user true 2>/dev/null; then
            ok "user namespaces available"
        else
            warn "user namespaces present but creation failed"
            fix "sudo sysctl -w kernel.unprivileged_userns_clone=1  (Debian)"
            fix "sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0  (Ubuntu 24.04)"
        fi
    else
        ok "user namespace file present (creation test needs root or unprivileged unshare)"
    fi
else
    warn "kernel has no user namespaces (optional --userns unavailable)"
fi
echo

# --- cgroup v2 --------------------------------------------------------------
echo "-- cgroup v2 --"
CGROOT="$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)"
if [ -z "$CGROOT" ]; then
    fail "cgroup v2 is not mounted — BoxForge cannot enforce memory/cpu limits"
    if [ $WSL -eq 1 ]; then
        fix "add to %UserProfile%\\.wslconfig:  [wsl2]"
        fix "  kernelCommandLine = cgroup_no_v1=all"
        fix "then run: wsl --shutdown"
    else
        fix "boot with systemd.unified_cgroup_hierarchy=1"
        fix "or: sudo mount -t cgroup2 none /sys/fs/cgroup"
    fi
else
    ok "cgroup v2 mounted at $CGROOT"

    # Check controllers
    ctl="$(cat "$CGROOT/cgroup.controllers" 2>/dev/null || echo "")"
    for c in cpu memory; do
        if echo " $ctl " | grep -q " $c "; then
            ok "$c controller available"
        else
            warn "$c controller NOT available (have: ${ctl:-none})"
            fix "use a pure cgroup v2 host (systemd.unified_cgroup_hierarchy=1)"
            fix "on WSL2: cgroup_no_v1=all in .wslconfig"
        fi
    done

    # Test cgroup writability
    if [ $IS_ROOT -eq 1 ]; then
        TEST_CG="$CGROOT/boxforge-preflight-$$"
        if mkdir "$TEST_CG" 2>/dev/null; then
            ok "can create cgroup under $CGROOT"

            # Test writing cpu.max
            CPU_CTL="$(cat "$CGROOT/cgroup.controllers" 2>/dev/null)"
            if echo " $CPU_CTL " | grep -q " cpu "; then
                # Enable cpu subtree control first
                echo "+cpu" > "$CGROOT/cgroup.subtree_control" 2>/dev/null || true
                if echo "20000 100000" > "$TEST_CG/cpu.max" 2>/dev/null; then
                    ok "can write cpu.max"
                else
                    fail "cannot write cpu.max in $TEST_CG"
                fi
            else
                skip "cpu controller not available — skipping cpu.max write test"
            fi

            # Test writing memory.max
            if echo " $CPU_CTL " | grep -q " memory "; then
                echo "+memory" > "$CGROOT/cgroup.subtree_control" 2>/dev/null || true
                if echo "67108864" > "$TEST_CG/memory.max" 2>/dev/null; then
                    ok "can write memory.max"
                else
                    fail "cannot write memory.max in $TEST_CG"
                fi
            else
                skip "memory controller not available — skipping memory.max write test"
            fi

            rmdir "$TEST_CG" 2>/dev/null || true
        else
            fail "cannot create $CGROOT/boxforge-preflight-$$ (read-only cgroup mount?)"
        fi
        # Clean up parent boxforge dir if it was created by us and is empty
        rmdir "$CGROOT/boxforge" 2>/dev/null || true
    else
        warn "not root: skipping cgroup write tests (cpu.max, memory.max)"
    fi
fi
echo

# --- overlayfs --------------------------------------------------------------
echo "-- Filesystem Features --"
if grep -qw overlay /proc/filesystems; then
    ok "overlayfs available"
else
    warn "overlayfs not available (optional --overlay)"
    fix "sudo modprobe overlay"
fi
echo

# --- rootfs -----------------------------------------------------------------
echo "-- Rootfs --"
if [ -x "$ROOTFS/bin/sh" ] || [ -L "$ROOTFS/bin/sh" ]; then
    if [ -x "$ROOTFS/bin/busybox" ] && ! ldd "$ROOTFS/bin/busybox" >/dev/null 2>&1; then
        ok "BusyBox rootfs at $ROOTFS (static busybox — preferred)"
    elif [ -x "$ROOTFS/bin/busybox" ]; then
        warn "rootfs busybox is dynamically linked: libraries must exist inside the rootfs"
    else
        warn "rootfs at $ROOTFS has no BusyBox"
        bad=0
        for lib in $(ldd "$ROOTFS/bin/sh" 2>/dev/null | grep -o '/[^ ]*'); do
            [ -e "$ROOTFS$lib" ] || { bad=1; warn "rootfs is missing $lib needed by /bin/sh"; }
        done
        [ $bad -eq 0 ] && ok "rootfs /bin/sh shared-library dependencies present"
    fi
    # Check required directories
    for d in bin etc lib lib64 usr dev proc tmp; do
        if [ -d "$ROOTFS/$d" ]; then
            ok "rootfs/$d exists"
        else
            warn "rootfs/$d missing (may cause issues)"
        fi
    done
else
    warn "no rootfs at $ROOTFS (bin/sh missing)"
    fix "sudo ./scripts/create-rootfs.sh"
fi
echo

# --- binary -----------------------------------------------------------------
echo "-- BoxForge Binary --"
if [ -x "$ROOT/boxforge" ]; then
    ok "boxforge binary built ($ROOT/boxforge)"
else
    info "boxforge not built yet — run 'make'"
fi
echo

# --- summary ----------------------------------------------------------------
echo "=== Pre-flight Summary ==="
if [ $fails -eq 0 ] && [ $warns -eq 0 ]; then
    echo "ALL CHECKS PASSED — ready to build and run BoxForge."
    exit 0
elif [ $fails -eq 0 ]; then
    echo "No blocking problems ($warns warning(s)) — BoxForge should work."
    echo "Address warnings for full functionality."
    exit 0
else
    echo "$fails blocking problem(s), $warns warning(s) — fix [BLOCKED] items before running."
    exit 1
fi
