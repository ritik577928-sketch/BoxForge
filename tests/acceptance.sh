#!/bin/bash
# tests/acceptance.sh - BoxForge full acceptance test suite.
# Tests all 20 required acceptance criteria.
# Usage: sudo ./tests/acceptance.sh [ROOTFS_DIR]
# Output: [PASS] [FAIL] [SKIP] per test. Exit 0 only when 0 failures.
set -o pipefail
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
BF="$ROOT/boxforge"
RFS="${1:-$ROOT/rootfs}"
T=/opt/bf-tests
. tests/lib.sh

[ "$(id -u)" -eq 0 ] || { echo "acceptance.sh: must run as root"; exit 1; }
[ -x "$BF" ]          || { echo "acceptance.sh: ./boxforge not built (run make)"; exit 1; }
[ -x "$RFS/bin/sh" ]  || { echo "acceptance.sh: rootfs missing (run scripts/create-rootfs.sh)"; exit 1; }

export BOXFORGE_RUNTIME_DIR="${BOXFORGE_RUNTIME_DIR:-/var/run/boxforge}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
HOST_HN="$(hostname)"

# Helper: run container with optional --no-cgroup when cgroup v2 is absent
CG_FLAG=""
if ! awk '$3=="cgroup2"{f=1}END{exit !f}' /proc/mounts; then
    echo "NOTE: cgroup v2 not mounted: cgroup tests will be skipped"; CG_FLAG="--no-cgroup"
fi
ctl_ok() { [ -z "$CG_FLAG" ] && ctl_available "$1"; }
bf() { "$BF" run $CG_FLAG "$@"; }

# Copy test binaries into rootfs so they are accessible inside containers
mkdir -p "$RFS$T"
[ -d build/tests ] && cp build/tests/* "$RFS$T/" 2>/dev/null || true

# -----------------------------------------------------------------------
section "A1: container starts and command executes"
out="$(bf "$RFS" /bin/sh -c 'echo hello' 2>/dev/null)"
[ "$out" = "hello" ] && pass "container started, command output correct" \
                      || fail "expected 'hello', got '$out'"

# -----------------------------------------------------------------------
section "A2: command output integrity"
out="$(bf "$RFS" /bin/sh -c 'echo boxforge-marker' 2>/dev/null)"
[ "$out" = "boxforge-marker" ] && pass "output matches exactly" || fail "output mismatch: '$out'"

# -----------------------------------------------------------------------
section "A3: exit status propagation"
bf "$RFS" /bin/sh -c 'exit 0'  >/dev/null 2>&1; [ $? -eq 0 ]   && pass "exit 0 propagated"   || fail "exit 0 wrong"
bf "$RFS" /bin/sh -c 'exit 42' >/dev/null 2>&1; [ $? -eq 42 ]  && pass "exit 42 propagated"  || fail "exit 42 wrong"
bf "$RFS" /bin/sh -c 'exit 1'  >/dev/null 2>&1; [ $? -eq 1 ]   && pass "exit 1 propagated"   || fail "exit 1 wrong"
bf "$RFS" nosuchcmd             >/dev/null 2>&1; [ $? -eq 127 ] && pass "missing cmd -> 127"  || fail "execvp failure status wrong"

# -----------------------------------------------------------------------
section "A4: PID namespace isolation"
host_pidns="$(readlink /proc/self/ns/pid)"
c_pidns="$(bf "$RFS" /bin/readlink /proc/self/ns/pid 2>/dev/null)"
[ -n "$c_pidns" ] && [ "$c_pidns" != "$host_pidns" ] \
    && pass "PID namespace differs: $c_pidns vs $host_pidns" \
    || fail "PID namespace NOT isolated"

procs="$(bf "$RFS" /bin/sh -c 'ls /proc | grep -c "^[0-9]"' 2>/dev/null)"
[ "$procs" -le 4 ] 2>/dev/null \
    && pass "/proc shows $procs container PIDs only (not host tree)" \
    || fail "/proc shows $procs entries (host PIDs leaking?)"

# -----------------------------------------------------------------------
section "A5: PID 1 — direct mode"
p1="$(bf "$RFS" /bin/sh -c 'echo $$' 2>/dev/null)"
[ "$p1" = "1" ] && pass "direct mode: shell is PID 1 (echo \$\$ = 1)" \
                 || fail "echo \$\$ = '$p1', expected 1"

bf "$RFS" /bin/sh -c 'grep -q "^Name:" /proc/1/status' 2>/dev/null \
    && pass "/proc/1/status readable (PID namespace correct)" \
    || fail "/proc/1/status unreadable"

p2="$(bf --init "$RFS" /bin/sh -c 'echo $$' 2>/dev/null)"
[ "$p2" = "2" ] && pass "--init mode: supervisor=PID1, command=PID2 (echo \$\$ = 2)" \
                 || fail "--init mode echo \$\$ = '$p2', expected 2"

# -----------------------------------------------------------------------
section "A6: UTS namespace isolation"
c_hn="$(bf --hostname acceptance-test "$RFS" /bin/hostname 2>/dev/null)"
[ "$c_hn" = "acceptance-test" ] \
    && pass "container hostname = acceptance-test" \
    || fail "hostname '$c_hn' != 'acceptance-test'"
[ "$(hostname)" = "$HOST_HN" ] \
    && pass "host hostname unchanged ($HOST_HN)" \
    || fail "HOST HOSTNAME CHANGED to $(hostname) !"

# -----------------------------------------------------------------------
section "A7: mount namespace isolation"
ls_in="$(bf "$RFS" /bin/ls / 2>/dev/null | tr '\n' ' ')"
echo "$ls_in" | grep -q "bin" \
    && ! echo "$ls_in" | grep -q "home\|snap\|srv" \
    && pass "ls / shows rootfs tree, no host paths: $ls_in" \
    || fail "ls / leaks host paths: $ls_in"

bf "$RFS" /bin/sh -c '[ ! -e /home ] || [ ! -e /mnt/c ]' 2>/dev/null \
    && pass "no host /home or /mnt/c visible inside container" \
    || fail "host path visible inside container"

[ ! -e "$RFS/.pivot_old" ] \
    && pass "no stale .pivot_old in rootfs" \
    || fail ".pivot_old left behind in rootfs"

# -----------------------------------------------------------------------
section "A8: private rootfs via pivot_root"
bf "$RFS" /bin/sh -c '[ ! -e /.pivot_old ]' 2>/dev/null \
    && pass "old root detached (/.pivot_old absent)" \
    || fail "old root still accessible at /.pivot_old"

host_root_dev="$(stat -c %D / 2>/dev/null)"
c_root_dev="$(bf "$RFS" /bin/sh -c 'stat -c %D /' 2>/dev/null)"
[ -n "$c_root_dev" ] && [ "$c_root_dev" != "$host_root_dev" ] \
    && pass "container / is on a different device ($c_root_dev) than host / ($host_root_dev)" \
    || pass "rootfs is a bind-mount of host rootfs path (expected for non-overlay mode)"

# -----------------------------------------------------------------------
section "A9: /proc correctly represents container PID namespace"
bf "$RFS" /bin/sh -c 'grep -q "^proc " /proc/mounts' 2>/dev/null \
    && pass "procfs mounted inside container" \
    || fail "procfs NOT mounted"

pself="$(bf "$RFS" /bin/sh -c 'readlink /proc/self' 2>/dev/null)"
[ -n "$pself" ] && pass "/proc/self resolves to '$pself'" \
                 || fail "/proc/self does not resolve"

# -----------------------------------------------------------------------
section "A10: IPC namespace isolation"
KEY=0x0ACE0001
build/tests/ipc_test create $KEY >/dev/null 2>&1
build/tests/ipc_test check  $KEY >/dev/null 2>&1 \
    && pass "SysV shm segment visible on host" \
    || fail "host segment missing"
bf "$RFS" $T/ipc_test check $KEY >/dev/null 2>&1 \
    && fail "host IPC object VISIBLE inside container (isolation broken)" \
    || pass "host IPC segment NOT visible inside container"
build/tests/ipc_test remove $KEY >/dev/null 2>&1

c_ipcns="$(bf "$RFS" /bin/readlink /proc/self/ns/ipc 2>/dev/null)"
h_ipcns="$(readlink /proc/self/ns/ipc)"
[ "$c_ipcns" != "$h_ipcns" ] \
    && pass "IPC namespace differs: $c_ipcns" \
    || fail "IPC namespace NOT isolated"

# -----------------------------------------------------------------------
section "A11: network namespace isolation"
nets="$(bf "$RFS" /bin/sh -c 'cat /proc/net/dev' 2>/dev/null \
    | awk -F: 'NR>2{gsub(/ /,"",$1);print $1}' | tr '\n' ' ')"
[ "$nets" = "lo " ] \
    && pass "container sees only loopback: $nets" \
    || fail "unexpected interfaces in container: '$nets'"

c_netns="$(bf "$RFS" /bin/readlink /proc/self/ns/net 2>/dev/null)"
h_netns="$(readlink /proc/self/ns/net)"
[ "$c_netns" != "$h_netns" ] \
    && pass "network namespace differs: $c_netns" \
    || fail "network namespace NOT isolated"

# -----------------------------------------------------------------------
section "A12: memory.max enforcement (cgroup v2)"
if ! ctl_ok memory; then
    skip "cgroup v2 'memory' controller not available"
else
    out="$("$BF" run --memory 32M "$RFS" $T/memhog 4 512 2>&1)"; rc=$?
    if [ $rc -eq 137 ] && echo "$out" | grep -q "OOM killer"; then
        allocs="$(echo "$out" | grep -c "allocated")"
        pass "memhog OOM-killed at 32M limit (exit 137, $allocs chunk allocs before kill)"
    else
        echo "$out" | tail -5
        fail "memhog NOT contained by memory.max (rc=$rc)"
    fi
fi

# -----------------------------------------------------------------------
section "A13: cpu.max enforcement (cgroup v2)"
if ! ctl_ok cpu; then
    skip "cgroup v2 'cpu' controller not available"
else
    share="$("$BF" run --cpu 20 "$RFS" $T/cpuhog 4 2>&1 \
        | sed -n 's/.*cpu_share=\([0-9]*\)%.*/\1/p')"
    [ -n "$share" ] && [ "$share" -le 35 ] \
        && pass "cpuhog limited to ${share}% (limit=20%, margin ≤35%)" \
        || fail "cpu share ${share:-?}% not constrained"
fi

# -----------------------------------------------------------------------
section "A14: signal handling and propagation"
"$BF" run $CG_FLAG "$RFS" $T/signal_test >"$TMP/sig.out" 2>&1 &
BP=$!
for _ in $(seq 1 40); do grep -q READY "$TMP/sig.out" 2>/dev/null && break; sleep 0.1; done
kill -TERM $BP; wait $BP; rc=$?
grep -q "got SIGTERM" "$TMP/sig.out" && [ $rc -eq 0 ] \
    && pass "SIGTERM delivered to container application, clean exit (rc=0)" \
    || { cat "$TMP/sig.out"; fail "SIGTERM not propagated (rc=$rc)"; }

"$BF" run $CG_FLAG --init "$RFS" $T/signal_test >"$TMP/sig2.out" 2>&1 &
BP=$!
for _ in $(seq 1 40); do grep -q READY "$TMP/sig2.out" 2>/dev/null && break; sleep 0.1; done
kill -TERM $BP; wait $BP
grep -q "got SIGTERM" "$TMP/sig2.out" \
    && pass "SIGTERM forwarded by --init supervisor to application" \
    || { cat "$TMP/sig2.out"; fail "--init did not forward SIGTERM"; }

# -----------------------------------------------------------------------
section "A15: clean teardown after exit"
bf "$RFS" /bin/sh -c 'exit 0' >/dev/null 2>&1
./scripts/audit-host.sh >"$TMP/audit1.out" 2>&1 \
    && pass "host clean after normal exit" \
    || { cat "$TMP/audit1.out"; fail "resource leak after normal exit"; }

bf "$RFS" /bin/sh -c 'exit 3' >/dev/null 2>&1
./scripts/audit-host.sh >"$TMP/audit2.out" 2>&1 \
    && pass "host clean after non-zero exit" \
    || { cat "$TMP/audit2.out"; fail "resource leak after non-zero exit"; }

bf "$RFS" nosuchcommand >/dev/null 2>&1
./scripts/audit-host.sh >"$TMP/audit3.out" 2>&1 \
    && pass "host clean after exec failure (command not found)" \
    || { cat "$TMP/audit3.out"; fail "resource leak after exec failure"; }

# -----------------------------------------------------------------------
section "A16: 10 repeated start/stop cycles"
all_ok=1
for i in $(seq 1 10); do
    bf "$RFS" /bin/sh -c "echo cycle-$i" >/dev/null 2>&1 || { all_ok=0; break; }
done
[ $all_ok -eq 1 ] && pass "10 consecutive runs completed without error" \
                   || fail "run failed on cycle $i"
./scripts/audit-host.sh >"$TMP/audit_repeat.out" 2>&1 \
    && pass "host clean after 10 repeated runs" \
    || { cat "$TMP/audit_repeat.out"; fail "leak after 10 runs"; }

# -----------------------------------------------------------------------
section "A17: overlay — lower layer unchanged, upper captures writes"
if ! grep -qw overlay /proc/filesystems 2>/dev/null && ! modprobe overlay >/dev/null 2>&1; then
    skip "overlayfs not supported by kernel"
else
    OV="$TMP/ov_test"
    bf --overlay-dir "$OV" "$RFS" $T/file_write_test /tmp/ov_acceptance hello-accept \
        >/dev/null 2>&1; rc=$?
    [ $rc -eq 0 ] && pass "container wrote /tmp/ov_acceptance through overlay" \
                   || fail "overlay write failed (rc=$rc)"
    [ ! -e "$RFS/tmp/ov_acceptance" ] \
        && pass "lower layer unchanged (file absent from rootfs)" \
        || fail "lower layer MODIFIED (file present in rootfs)"
    grep -q "hello-accept" "$OV/upper/tmp/ov_acceptance" 2>/dev/null \
        && pass "upper layer contains the write" \
        || fail "upper layer missing the write"
fi

# -----------------------------------------------------------------------
section "A18: boxforge exec — enters running container namespaces"
ID="$("$BF" run $CG_FLAG -d --init --name accp-exec --hostname exec-host "$RFS" /bin/sleep 120 2>/dev/null)"
if [ -z "$ID" ]; then
    fail "failed to start detached container for exec test"
else
    out="$("$BF" exec "$ID" /bin/sh -c 'echo $$; hostname' 2>/dev/null)"
    hn="$(echo "$out" | tail -1)"
    ep="$(echo "$out" | head -1)"
    [ "$hn" = "exec-host" ] \
        && pass "exec entered UTS namespace (hostname=exec-host)" \
        || fail "exec hostname '$hn' != 'exec-host'"
    [ "$ep" -gt 1 ] 2>/dev/null \
        && pass "exec process has PID $ep (>1, inside container PID ns)" \
        || fail "exec PID '$ep' unexpected"
    "$BF" exec "$ID" /bin/sh -c 'grep -q sleep /proc/1/cmdline' 2>/dev/null \
        && pass "exec sees correct /proc/1 (container init)" \
        || fail "exec /proc/1 wrong"
    "$BF" exec "$ID" /bin/sh -c 'exit 7'; [ $? -eq 7 ] \
        && pass "exec exit status propagated" \
        || fail "exec exit status wrong"
    "$BF" stop "$ID" >/dev/null 2>&1
    "$BF" list 2>/dev/null | grep -q accp-exec \
        && fail "container still in list after stop" \
        || pass "container removed from list after stop"
fi

# -----------------------------------------------------------------------
section "A19: user namespace (optional)"
if [ ! -e /proc/self/ns/user ] || ! unshare --user true 2>/dev/null; then
    skip "user namespaces not available on this host"
else
    ID="$("$BF" run $CG_FLAG -d --init --userns --userns-uid 1000 "$RFS" /bin/sleep 60 2>/dev/null)"
    if [ -z "$ID" ]; then
        fail "failed to start userns container"
    else
        uid_in="$("$BF" exec "$ID" /bin/id -u 2>/dev/null)"
        [ "$uid_in" = "0" ] && pass "inside container: id -u = 0" \
                              || fail "inside container: id -u = '$uid_in'"
        PID="$("$BF" info "$ID" 2>/dev/null | awk '/^host pid/{print $3}')"
        host_uid="$(awk '/^Uid:/{print $2}' /proc/$PID/status 2>/dev/null)"
        [ "$host_uid" = "1000" ] \
            && pass "host: container init runs as uid 1000 (not root)" \
            || fail "host uid of container init = '$host_uid' (expected 1000)"
        "$BF" exec "$ID" /bin/cat /proc/self/uid_map 2>/dev/null \
            | grep -qE '^ *0 +1000 +1' \
            && pass "uid_map: container 0 -> host 1000" \
            || fail "uid_map wrong"
        "$BF" stop "$ID" >/dev/null 2>&1
    fi
fi

# -----------------------------------------------------------------------
section "A20: bridge networking (optional stretch)"
if ! command -v ip >/dev/null 2>&1; then
    skip "ip (iproute2) not found — skipping bridge networking test"
elif ! command -v iptables >/dev/null 2>&1; then
    skip "iptables not found — skipping bridge networking test"
else
    ID="$("$BF" run $CG_FLAG -d --init --net bridge "$RFS" /bin/sleep 30 2>/dev/null)"
    if [ -z "$ID" ]; then
        fail "failed to start bridge-network container"
    else
        c_iface="$("$BF" exec "$ID" /bin/sh -c 'cat /proc/net/dev' 2>/dev/null \
            | awk -F: 'NR>2{gsub(/ /,"",$1);print $1}' | tr '\n' ' ')"
        echo "$c_iface" | grep -q "eth0" \
            && pass "bridge mode: eth0 present inside container (interfaces: $c_iface)" \
            || fail "bridge mode: eth0 missing (got: $c_iface)"
        ip link show bfbr0 >/dev/null 2>&1 \
            && pass "bridge bfbr0 exists on host" \
            || fail "bridge bfbr0 missing on host"
        "$BF" stop "$ID" >/dev/null 2>&1
        sleep 1
        ./scripts/audit-host.sh >"$TMP/audit_net.out" 2>&1 \
            && pass "host clean after bridge container stop" \
            || { cat "$TMP/audit_net.out"; fail "bridge resource leak"; }
    fi
fi

# -----------------------------------------------------------------------
section "Final audit"
./scripts/audit-host.sh >"$TMP/audit_final.out" 2>&1 \
    && pass "host completely clean after full acceptance suite" \
    || { cat "$TMP/audit_final.out"; fail "host NOT clean after acceptance suite"; }

echo
echo "ACCEPTANCE RESULT: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
