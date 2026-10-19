#!/bin/bash
# run_tests.sh - BoxForge automated test suite (needs root). Run via `make test`.
# Tests that need a kernel feature this host lacks are reported as SKIP with the
# reason - never as PASS.
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
. tests/lib.sh
BF="$ROOT/boxforge"
RFS="${ROOTFS:-$ROOT/rootfs}"
T=/opt/bf-tests
[ "$(id -u)" -eq 0 ] || { echo "run as root (make test does this via sudo)"; exit 1; }
export BOXFORGE_RUNTIME_DIR="${BOXFORGE_RUNTIME_DIR:-/var/run/boxforge}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
HOST_HOSTNAME="$(hostname)"

section "Test 1: build"
if make -s all testbins >"$TMP/build.log" 2>&1 && [ -x "$BF" ]; then pass "make succeeded"; else cat "$TMP/build.log"; fail "build failed"; exit 1; fi
[ -x "$RFS/bin/sh" ] || ./scripts/create-rootfs.sh --dir "$RFS" >/dev/null 2>&1 || ./scripts/create-rootfs.sh --dir "$RFS" --host-tools >/dev/null 2>&1
[ -x "$RFS/bin/sh" ] || { fail "no rootfs (run scripts/create-rootfs.sh)"; exit 1; }
mkdir -p "$RFS$T" && cp build/tests/* "$RFS$T/"
CG_FLAG=""
if ! awk '$3=="cgroup2"{f=1} END{exit !f}' /proc/mounts; then
    echo "NOTE: cgroup v2 not mounted: running with --no-cgroup (cgroup tests skipped)"; CG_FLAG="--no-cgroup"; fi
bf() { "$BF" run $CG_FLAG "$@"; }

section "Test 2: executes /bin/sh"
out="$(bf "$RFS" /bin/sh -c 'echo hello' 2>&1)"; [ "$out" = "hello" ] && pass "sh -c echo hello" || fail "got '$out'"
bf "$RFS" /bin/ls / >/dev/null 2>&1 && pass "/bin/ls runs" || fail "/bin/ls failed"

section "Test 3: exit status"
bf "$RFS" /bin/sh -c 'exit 7' >/dev/null 2>&1; [ $? -eq 7 ] && pass "exit 7 propagated" || fail "exit status wrong"
bf "$RFS" /bin/sh -c 'exit 0' >/dev/null 2>&1; [ $? -eq 0 ] && pass "exit 0 propagated" || fail "exit 0 wrong"
bf "$RFS" nosuchcmd >/dev/null 2>&1; [ $? -eq 127 ] && pass "missing command -> 127" || fail "execvp failure status wrong"

section "Test 4: PID isolation"
n="$(bf "$RFS" /bin/ps 2>/dev/null | grep -c -v PID)"
[ "$n" -le 2 ] && pass "ps inside shows $n process(es), not the host tree" || fail "ps shows $n processes"
host_pid_ns="$(readlink /proc/self/ns/pid)"; c_pid_ns="$(bf "$RFS" /bin/readlink /proc/self/ns/pid 2>/dev/null)"
[ -n "$c_pid_ns" ] && [ "$c_pid_ns" != "$host_pid_ns" ] && pass "pid namespace differs ($c_pid_ns vs $host_pid_ns)" || fail "pid ns not isolated"

section "Test 5: PID 1"
[ "$(bf "$RFS" /bin/sh -c 'echo $$' 2>/dev/null)" = "1" ] && pass "direct mode: command is PID 1" || fail "echo \$\$ != 1"
bf "$RFS" /bin/sh -c 'grep -q "^Name:.*sh" /proc/1/status; r=$?; exit $r' 2>/dev/null && pass "/proc/1/status is the shell" || fail "/proc/1/status"
[ "$(bf --init "$RFS" /bin/sh -c 'echo $$' 2>/dev/null)" = "2" ] && pass "--init mode: supervisor is PID 1, command PID 2" || fail "--init pid layout"
z="$(bf --init "$RFS" $T/fork_tree orphan 2>/dev/null | sed -n 's/.*zombies=\([0-9]*\).*/\1/p')"
[ "$z" = "0" ] && pass "--init reaps orphans (zombies=$z)" || fail "--init left $z zombie(s)"
z="$(bf "$RFS" $T/fork_tree orphan 2>/dev/null | sed -n 's/.*zombies=\([0-9]*\).*/\1/p')"
[ -n "$z" ] && [ "$z" -ge 1 ] && pass "direct mode documented tradeoff confirmed (target as PID 1 left $z zombie)" || fail "expected zombie in direct mode, got '$z'"

section "Test 6: UTS isolation"
[ "$(bf --hostname forge-test "$RFS" /bin/hostname 2>/dev/null)" = "forge-test" ] && pass "container hostname = forge-test" || fail "hostname"
[ "$(hostname)" = "$HOST_HOSTNAME" ] && pass "host hostname unchanged ($HOST_HOSTNAME)" || fail "host hostname changed!"

section "Test 7: filesystem isolation"
ls_in="$(bf "$RFS" /bin/ls / 2>/dev/null | tr '\n' ' ')"
echo "$ls_in" | grep -q "bin" && ! echo "$ls_in" | grep -qE "\b(home|mnt|run|snap|srv)\b" && pass "ls / = rootfs tree: $ls_in" || fail "ls / shows '$ls_in'"
bf "$RFS" /bin/sh -c '[ ! -e /home/claude ] && [ ! -e /.pivot_old ]' 2>/dev/null && pass "host paths and old-root mount point absent" || fail "host path visible"
bf "$RFS" /bin/sh -c 'cat /proc/mounts' 2>/dev/null | grep -q "boxforge" && fail "host path leaks in /proc/mounts" || pass "no host paths in container mount table"
[ ! -e "$RFS/.pivot_old" ] && pass "no .pivot_old left in rootfs" || fail ".pivot_old left"

section "Test 8: /proc"
bf "$RFS" /bin/sh -c 'grep -q "^proc " /proc/mounts' 2>/dev/null && pass "proc mounted" || fail "proc not mounted"
[ "$(bf "$RFS" /bin/sh -c 'readlink /proc/self | head -c 1' 2>/dev/null)" != "" ] && pass "/proc/self resolves (PID-aware)" || fail "/proc/self"
bf "$RFS" /bin/sh -c 'ls /proc | grep -c "^[0-9]" ' 2>/dev/null | awk '{exit !($1<=4)}' && pass "/proc lists only container PIDs" || fail "/proc lists host PIDs"

section "Test 9: memory limit (cgroup v2 memory.max)"
if [ -n "$CG_FLAG" ] || ! ctl_available memory; then skip "cgroup v2 'memory' controller not available on this host (see preflight)"
else
    out="$("$BF" run --memory 32M "$RFS" $T/memhog 4 512 2>&1)"; rc=$?
    if [ $rc -eq 137 ] && echo "$out" | grep -q "OOM killer"; then pass "memhog OOM-killed at 32M (exit 137): $(echo "$out" | grep -c allocated) allocations"; else echo "$out" | tail -3; fail "memhog not contained (rc=$rc)"; fi
    [ "$(hostname)" = "$HOST_HOSTNAME" ] && free -m >/dev/null && pass "host remains healthy"
fi

section "Test 10: CPU limit (cgroup v2 cpu.max)"
if [ -n "$CG_FLAG" ] || ! ctl_available cpu; then skip "cgroup v2 'cpu' controller not available on this host (see preflight)"
else
    s="$("$BF" run --cpu 20 "$RFS" $T/cpuhog 4 2>&1 | sed -n 's/.*cpu_share=\([0-9]*\)%.*/\1/p')"
    [ -n "$s" ] && [ "$s" -le 35 ] && pass "cpuhog limited to ${s}% (limit 20%)" || fail "cpu share ${s:-?}% not limited"
fi

section "Test 11: IPC isolation"
KEY=0x0B0F0123
build/tests/ipc_test create $KEY >/dev/null
build/tests/ipc_test check $KEY >/dev/null && pass "segment visible on host" || fail "host segment missing"
bf "$RFS" $T/ipc_test check $KEY >/dev/null 2>&1 && fail "host IPC object visible in container" || pass "host shm segment NOT visible in container"
build/tests/ipc_test remove $KEY >/dev/null
[ "$(bf "$RFS" /bin/readlink /proc/self/ns/ipc 2>/dev/null)" != "$(readlink /proc/self/ns/ipc)" ] && pass "ipc namespace differs" || fail "ipc ns same"

section "Test 12: network namespace isolation"
nets="$(bf "$RFS" /bin/sh -c 'cat /proc/net/dev' 2>/dev/null | awk -F: 'NR>2{gsub(/ /,"",$1); print $1}' | tr '\n' ' ')"
[ "$nets" = "lo " ] && pass "container sees only: $nets" || fail "container interfaces: '$nets'"
[ "$(bf "$RFS" /bin/readlink /proc/self/ns/net 2>/dev/null)" != "$(readlink /proc/self/ns/net)" ] && pass "net namespace differs" || fail "net ns same"
bf "$RFS" /bin/sh -c 'grep -q "lo" /proc/net/dev && ! grep -q eth /proc/net/dev' 2>/dev/null && pass "no Internet-capable interface (netns alone gives no connectivity)" || fail "unexpected iface"
for ns in mnt uts; do
    [ "$(bf "$RFS" /bin/readlink /proc/self/ns/$ns 2>/dev/null)" != "$(readlink /proc/self/ns/$ns)" ] && pass "$ns namespace differs" || fail "$ns ns same"
done

section "Test 13: signal handling"
"$BF" run $CG_FLAG "$RFS" $T/signal_test >"$TMP/sig.out" 2>&1 & BP=$!
for _ in $(seq 1 30); do grep -q READY "$TMP/sig.out" 2>/dev/null && break; sleep 0.1; done
kill -TERM $BP; wait $BP; rc=$?
grep -q "got SIGTERM" "$TMP/sig.out" && [ $rc -eq 0 ] && pass "SIGTERM delivered to application (direct mode), clean exit" || { cat "$TMP/sig.out"; fail "SIGTERM not propagated rc=$rc"; }
"$BF" run $CG_FLAG --init "$RFS" $T/signal_test >"$TMP/sig2.out" 2>&1 & BP=$!
for _ in $(seq 1 30); do grep -q READY "$TMP/sig2.out" 2>/dev/null && break; sleep 0.1; done
kill -TERM $BP; wait $BP; grep -q "got SIGTERM" "$TMP/sig2.out" && pass "SIGTERM forwarded by init supervisor" || fail "init did not forward SIGTERM"
ID="$("$BF" run $CG_FLAG -d --init "$RFS" /bin/sleep 300)"; s=$(date +%s)
"$BF" stop "$ID" >/dev/null; e=$(date +%s)
[ $((e-s)) -le 3 ] && pass "boxforge stop (graceful) took $((e-s))s" || fail "stop slow"
ID="$("$BF" run $CG_FLAG -d --stop-timeout 2 "$RFS" /bin/sh -c 'trap "" TERM; exec /bin/sleep 300')"; s=$(python3 -c 'import time; print(time.monotonic_ns())')
"$BF" stop "$ID" >/dev/null 2>&1; e=$(python3 -c 'import time; print(time.monotonic_ns())')
elapsed_ms=$(((e-s)/1000000))
[ "$elapsed_ms" -ge 2000 ] && [ "$elapsed_ms" -le 6500 ] && pass "SIGTERM-ignoring PID 1 escalated to SIGKILL after timeout (${elapsed_ms}ms)" || fail "escalation timing ${elapsed_ms}ms"

section "Test 14: cleanup (10 runs)"
for i in $(seq 1 10); do bf "$RFS" /bin/sh -c "exit 0" >/dev/null 2>&1; done
bf "$RFS" /bin/sh -c 'exit 3' >/dev/null 2>&1; bf "$RFS" nosuchcmd >/dev/null 2>&1
ID="$("$BF" run $CG_FLAG -d --init "$RFS" /bin/sleep 100)"; "$BF" stop "$ID" >/dev/null
if ./scripts/audit-host.sh >"$TMP/audit.out" 2>&1; then pass "audit-host.sh: no cgroups, mounts, veth, runtime state or processes left"; else cat "$TMP/audit.out"; fail "resource leak detected"; fi
[ -z "$(ls -A "$RFS/dev" 2>/dev/null)" ] && pass "rootfs/dev left empty (tmpfs was private)" || fail "rootfs/dev polluted"

section "Test 15: overlay"
if ! grep -qw overlay /proc/filesystems 2>/dev/null && ! modprobe overlay >/dev/null 2>&1; then
    skip "overlayfs not supported by kernel"
else
    OV="$TMP/ov"; before="$(find "$RFS" -path "$RFS/opt" -prune -o -type f -newer "$BF" -print 2>/dev/null | wc -l)"
    bf --overlay-dir "$OV" "$RFS" $T/file_write_test /tmp/ovfile hello-overlay >/dev/null 2>&1; rc=$?
    [ $rc -eq 0 ] && pass "container wrote /tmp/ovfile through the overlay" || fail "overlay run rc=$rc"
    [ ! -e "$RFS/tmp/ovfile" ] && pass "lower layer unchanged (no tmp/ovfile)" || fail "lower layer modified!"
    grep -q hello-overlay "$OV/upper/tmp/ovfile" 2>/dev/null && pass "upper layer contains the write" || fail "upper missing write"
    [ ! -e "$OV/work" ] && pass "overlay work dir cleaned up" || fail "work dir left"
    bf --overlay "$RFS" /bin/sh -c 'rm -f /bin/ls; touch /tmp/e' >/dev/null 2>&1; [ -x "$RFS/bin/ls" ] && pass "deleting /bin/ls in container does not touch base layer" || fail "base layer damaged"
fi

section "Extra: boxforge exec / list / info / stop"
ID="$("$BF" run $CG_FLAG -d --init --name exectest --hostname exechost "$RFS" /bin/sleep 120)"
"$BF" list | grep -q exectest && pass "list shows container" || fail "list"
"$BF" info exectest | grep -q "state:       running" && pass "info shows running state" || fail "info"
out="$("$BF" exec exectest /bin/sh -c 'echo $$; hostname' 2>&1)"
[ "$(echo "$out" | tail -1)" = "exechost" ] && [ "$(echo "$out" | head -1)" -gt 1 ] && pass "exec entered UTS+PID namespaces (child pid $(echo "$out" | head -1) in container pid ns)" || fail "exec output: $out"
"$BF" exec exectest /bin/sh -c 'grep -q sleep /proc/1/cmdline' && pass "exec sees container PID 1 (sleep) via its /proc" || fail "exec pid ns"
[ "$("$BF" exec exectest /bin/readlink /proc/self/ns/net)" = "$("$BF" info exectest | awk '$1=="net"{print $2}')" ] && pass "exec shares container network namespace" || fail "exec net ns"
"$BF" exec exectest /bin/sh -c 'exit 5'; [ $? -eq 5 ] && pass "exec propagates exit status" || fail "exec status"
"$BF" stop exectest >/dev/null && ! "$BF" list | grep -q exectest && pass "stop removed container" || fail "stop"

section "Optional: user namespace"
if [ ! -e /proc/self/ns/user ] || ! unshare --user true 2>/dev/null; then skip "user namespaces unavailable"
else
    ID="$("$BF" run $CG_FLAG -d --init --userns --userns-uid 1000 "$RFS" /bin/sleep 60 2>&1)"
    PID="$("$BF" info "$ID" 2>/dev/null | awk '/^host pid/{print $3}')"
    [ "$("$BF" exec "$ID" /bin/id -u 2>/dev/null)" = "0" ] && pass "inside: id -u = 0" || fail "inside uid"
    [ "$(awk '/^Uid:/{print $2}' /proc/$PID/status 2>/dev/null)" = "1000" ] && pass "host: container init runs as uid 1000 (not root)" || fail "host uid"
    "$BF" exec "$ID" /bin/cat /proc/self/uid_map 2>/dev/null | grep -qE '^ +0 +1000 +1' && pass "uid_map: 0 -> 1000" || fail "uid_map"
    "$BF" stop "$ID" >/dev/null
fi

section "Final audit"
./scripts/audit-host.sh >"$TMP/audit2.out" 2>&1 && pass "host clean after full suite" || { cat "$TMP/audit2.out"; fail "leaks after suite"; }

echo; echo "RESULT: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
