#!/bin/bash
# test_network.sh - STRETCH: veth + bridge + NAT networking (--net bridge).
cd "$(dirname "$0")/.." || exit 1
. tests/lib.sh
BF="$PWD/boxforge"; RFS="${ROOTFS:-$PWD/rootfs}"
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }
export BOXFORGE_RUNTIME_DIR="${BOXFORGE_RUNTIME_DIR:-/var/run/boxforge}"
for c in ip; do command -v $c >/dev/null || { echo "SKIP: '$c' missing (sudo apt install iproute2)"; exit 77; }; done
CG=""; awk '$3=="cgroup2"{f=1} END{exit !f}' /proc/mounts || CG="--no-cgroup"
mkdir -p "$RFS/opt/bf-tests" && cp build/tests/* "$RFS/opt/bf-tests/" 2>/dev/null

section "bridge networking"
ID="$("$BF" run $CG -d --init --net bridge "$RFS" /bin/sleep 120)" || { fail "start failed"; exit 1; }
IP="$("$BF" info "$ID" | awk '/^ip:/{print $2}')"
[ -n "$IP" ] && pass "container got $IP" || fail "no ip"
ip link show bfbr0 >/dev/null 2>&1 && pass "bridge bfbr0 exists" || fail "bridge missing"
"$BF" exec "$ID" /bin/cat /proc/net/dev | grep -q eth0 && pass "eth0 present in container" || fail "eth0 missing"
ping -c1 -W2 "$IP" >/dev/null 2>&1 && pass "host -> container ping" || fail "host cannot ping container"
"$BF" exec "$ID" /bin/ping -c1 -W2 10.88.0.1 >/dev/null 2>&1 && pass "container -> host (bridge IP) ping" || skip "container has no ping binary or ICMP failed"
if [ -n "$NET_INTERNET" ]; then
    "$BF" exec "$ID" /bin/ping -c1 -W3 1.1.1.1 >/dev/null 2>&1 && pass "outbound Internet via NAT" || fail "no Internet via NAT"
else skip "Internet test (set NET_INTERNET=1 to enable)"; fi
"$BF" stop "$ID" >/dev/null
ip link show 2>/dev/null | grep -q -E "bfh|bfc" && fail "veth left behind" || pass "veth removed"
ip link show bfbr0 >/dev/null 2>&1 && fail "bridge left behind" || pass "bridge removed with last container"
./scripts/audit-host.sh >/dev/null && pass "audit clean" || fail "audit found leaks"
echo; echo "RESULT: $PASS passed, $FAIL failed, $SKIP skipped"; [ $FAIL -eq 0 ]
