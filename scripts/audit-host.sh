#!/bin/bash
# audit-host.sh - report any BoxForge resources left on the host.
# Usage: sudo ./scripts/audit-host.sh
# Exit 0 if clean, exit 1 if leaks found.
# Honors BOXFORGE_RUNTIME_DIR (default /var/run/boxforge).
RT="${BOXFORGE_RUNTIME_DIR:-/var/run/boxforge}"
leaks=0
leak()   { printf "[LEAK]  %s\n" "$*"; leaks=$((leaks+1)); }
ok()     { printf "[OK]    %s\n" "$*"; }
info()   { printf "[INFO]  %s\n" "$*"; }

echo "=== BoxForge Host Audit ==="
echo

# --- cgroups ----------------------------------------------------------------
echo "-- cgroup v2 --"
CGROOT="$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)"
n=0
if [ -n "$CGROOT" ] && [ -d "$CGROOT/boxforge" ]; then
    for d in "$CGROOT"/boxforge/*/; do
        [ -d "$d" ] || continue
        leak "leftover cgroup: $d"
        # Show if it has any processes still
        procs="$(cat "$d/cgroup.procs" 2>/dev/null | wc -l)"
        [ "$procs" -gt 0 ] && info "  -> $procs process(es) still in cgroup"
        n=1
    done
fi
[ $n -eq 0 ] && ok "no leftover BoxForge cgroups in ${CGROOT:-/sys/fs/cgroup}/boxforge"
echo

# --- mounts -----------------------------------------------------------------
echo "-- Mounts --"
n=0
while IFS= read -r line; do
    leak "leftover mount: $line"
    n=1
done < <(grep -E "\.pivot_old|/boxforge/[0-9a-f]{12}|$RT" /proc/mounts 2>/dev/null)
[ $n -eq 0 ] && ok "no leftover BoxForge mounts"
echo

# --- network interfaces ------------------------------------------------------
echo "-- Network Interfaces --"
n=0
for i in /sys/class/net/bfh* /sys/class/net/bfc*; do
    [ -e "$i" ] || continue
    iface="$(basename "$i")"
    leak "leftover veth interface: $iface"
    n=1
done

if [ -e /sys/class/net/bfbr0 ]; then
    if [ -d /sys/class/net/bfbr0/brif ]; then
        members=$(ls /sys/class/net/bfbr0/brif/ 2>/dev/null | wc -l)
        if [ "$members" -eq 0 ]; then
            leak "empty bridge bfbr0 (no members but still exists)"
            n=1
        else
            # Bridge with members is expected during a running container
            info "bridge bfbr0 has $members member(s) — may be in use"
        fi
    fi
fi
[ $n -eq 0 ] && ok "no leftover veth/bridge interfaces"
echo

# --- iptables (NAT/FORWARD) -------------------------------------------------
echo "-- iptables Rules --"
n=0
if command -v iptables >/dev/null 2>&1; then
    # Check for our specific NAT rule (MASQUERADE for 10.88.0.0/24)
    if iptables -w -t nat -C POSTROUTING -s 10.88.0.0/24 '!' -o bfbr0 -j MASQUERADE 2>/dev/null; then
        leak "leftover iptables NAT rule: POSTROUTING MASQUERADE for 10.88.0.0/24"
        n=1
    fi
    if iptables -w -t filter -C FORWARD -i bfbr0 -j ACCEPT 2>/dev/null; then
        leak "leftover iptables FORWARD rule: -i bfbr0 ACCEPT"
        n=1
    fi
    if iptables -w -t filter -C FORWARD -o bfbr0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
        leak "leftover iptables FORWARD rule: -o bfbr0 RELATED,ESTABLISHED ACCEPT"
        n=1
    fi
    [ $n -eq 0 ] && ok "no leftover BoxForge iptables rules"
else
    ok "iptables not installed — skipping NAT/FORWARD check"
fi
echo

# --- runtime metadata -------------------------------------------------------
echo "-- Runtime Metadata --"
n=0
if [ -d "$RT" ]; then
    for d in "$RT"/*/; do
        [ -d "$d" ] || continue
        id="$(basename "$d")"
        state="$(sed -n 's/^state=//p' "$d/state" 2>/dev/null)"
        pid="$(sed -n 's/^pid=//p' "$d/state" 2>/dev/null)"
        mpid="$(sed -n 's/^monitor_pid=//p' "$d/state" 2>/dev/null)"
        leak "stale runtime metadata: $d (state=$state pid=$pid monitor_pid=$mpid)"
        n=1
    done
fi
[ $n -eq 0 ] && ok "no stale runtime metadata in $RT"
echo

# --- lingering processes ----------------------------------------------------
echo "-- BoxForge Processes --"
n=0
for p in /proc/[0-9]*; do
    pid="${p#/proc/}"
    [ "$pid" = "$$" ] && continue
    # Skip zombies
    st="$(awk '{print $3}' "$p/stat" 2>/dev/null | head -c1)"
    [ "$st" = "Z" ] && continue
    comm="$(cat "$p/comm" 2>/dev/null)"
    [ "$comm" = "boxforge" ] || continue
    leak "boxforge process still running: pid $pid"
    n=1
done
[ $n -eq 0 ] && ok "no lingering boxforge processes"
echo

# --- ip_forward state -------------------------------------------------------
echo "-- System State --"
fwd_marker="$RT/ip_forward.prev"
if [ -f "$fwd_marker" ]; then
    prev="$(cat "$fwd_marker" 2>/dev/null)"
    cur="$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null)"
    if [ "$prev" = "0" ] && [ "$cur" = "1" ]; then
        leak "ip_forward was changed to 1 but marker still exists (BoxForge may have crashed)"
    else
        ok "ip_forward state marker: prev=$prev cur=${cur:-?}"
    fi
else
    cur="$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null)"
    ok "ip_forward=$cur (no BoxForge marker)"
fi
echo

# --- summary ----------------------------------------------------------------
echo "=== Audit Summary ==="
if [ $leaks -eq 0 ]; then
    echo "audit: CLEAN — no BoxForge resource leaks found"
    exit 0
else
    echo "audit: $leaks leak(s) found"
    echo "       Run: sudo ./scripts/cleanup.sh"
    exit 1
fi
