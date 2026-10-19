#!/bin/bash
# cleanup.sh - force-remove leftovers of crashed BoxForge runs.
# Usage: sudo ./scripts/cleanup.sh [--all]    (--all also stops running containers)
RT="${BOXFORGE_RUNTIME_DIR:-/var/run/boxforge}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }

if [ "$1" = "--all" ] && [ -x "$ROOT/boxforge" ] && [ -d "$RT" ]; then
    for d in "$RT"/*/; do
        id="$(basename "$d")"
        [ -f "$d/state" ] && "$ROOT/boxforge" stop "$id" >/dev/null 2>&1
    done
fi

CGROOT="$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)"
if [ -n "$CGROOT" ] && [ -d "$CGROOT/boxforge" ]; then
    for d in "$CGROOT"/boxforge/*/; do
        [ -d "$d" ] || continue
        [ -w "$d/cgroup.kill" ] && echo 1 > "$d/cgroup.kill" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do rmdir "$d" 2>/dev/null && break; sleep 0.2; done
        [ -d "$d" ] && echo "could not remove $d" || echo "removed cgroup $d"
    done
fi

for i in /sys/class/net/bfh*; do
    [ -e "$i" ] && ip link del "$(basename "$i")" 2>/dev/null && echo "removed $(basename "$i")"
done
if [ -d /sys/class/net/bfbr0/brif ] && [ -z "$(ls -A /sys/class/net/bfbr0/brif)" ]; then
    ip link del bfbr0 2>/dev/null && echo "removed bridge bfbr0"
    rm -f "$RT/bridge.created"
fi

if [ -d "$RT" ]; then
    for d in "$RT"/*/; do
        [ -d "$d" ] || continue
        pid="$(sed -n 's/^pid=//p' "$d/state" 2>/dev/null)"
        mpid="$(sed -n 's/^monitor_pid=//p' "$d/state" 2>/dev/null)"
        if { [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; } && { [ -z "$mpid" ] || ! kill -0 "$mpid" 2>/dev/null; }; then
            umount -l "$d/merged" 2>/dev/null
            rm -rf "$d" && echo "removed stale runtime dir $d"
        fi
    done
fi
echo "cleanup finished"
