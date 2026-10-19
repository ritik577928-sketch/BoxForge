#!/bin/sh
# create-rootfs.sh - build a minimal root filesystem for BoxForge.
#
# Usage: sudo ./scripts/create-rootfs.sh [--dir DIR] [--busybox PATH] [--host-tools] [--force]
#
# Preferred: a statically linked BusyBox (found on the host, installed from the
# Ubuntu 'busybox-static' package, or downloaded).  --host-tools is an explicit
# fallback for machines with no network: it copies a few dynamically linked host
# programs plus their shared libraries (works, but is not a "pure" BusyBox rootfs).
set -eu

DIR="$(cd "$(dirname "$0")/.." && pwd)/rootfs"
BUSYBOX=""
HOST_TOOLS=0
FORCE=0
BB_URL="https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"

die() { echo "[FAIL] $*" >&2; exit 1; }
ok()  { echo "[OK] $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --busybox) BUSYBOX="$2"; shift 2 ;;
        --host-tools) HOST_TOOLS=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ "$(uname -s)" = "Linux" ] || die "Linux required"
for c in mkdir ln chmod cp; do command -v "$c" >/dev/null 2>&1 || die "missing required utility: $c"; done

if [ -e "$DIR/bin" ] && [ "$FORCE" -ne 1 ]; then
    if [ -x "$DIR/bin/busybox" ] || [ -x "$DIR/bin/sh" ]; then
        ok "rootfs already present at $DIR (use --force to rebuild)"
        exit 0
    fi
fi

mkdir -p "$DIR"/bin "$DIR"/sbin "$DIR"/etc "$DIR"/dev "$DIR"/proc "$DIR"/sys "$DIR"/tmp \
         "$DIR"/root "$DIR"/usr/bin "$DIR"/usr/sbin "$DIR"/lib "$DIR"/lib64 "$DIR"/var "$DIR"/opt
chmod 1777 "$DIR/tmp"
chmod 0700 "$DIR/root"

is_static() {
    # "not a dynamic executable" is what ldd prints for static binaries
    ldd "$1" 2>&1 | grep -q -E "not a dynamic executable|statically linked"
}

copy_with_libs() {
    # copy_with_libs <host-binary> -> installs into $DIR/bin and copies its libraries
    src="$1"
    [ -x "$src" ] || return 1
    name="$(basename "$src")"
    cp -L "$src" "$DIR/bin/$name"
    ldd "$src" 2>/dev/null | while read -r line; do
        lib=""
        case "$line" in
            *"=>"*"/"*) lib="$(echo "$line" | sed -n 's/.*=> *\(\/[^ ]*\).*/\1/p')" ;;
            /*) lib="$(echo "$line" | awk '{print $1}')" ;;
        esac
        [ -n "$lib" ] && [ -e "$lib" ] || continue
        mkdir -p "$DIR$(dirname "$lib")"
        cp -L "$lib" "$DIR$lib"
    done
}

if [ "$HOST_TOOLS" -eq 1 ]; then
    echo "[WARN] --host-tools: building a NON-BusyBox rootfs from dynamically linked host binaries"
    for t in bash ls cat ps hostname id sleep mkdir rm touch readlink wc grep seq head true false \
             echo cp mv stat uname env kill tr tail cut sort sed date sh; do
        [ "$t" = "sh" ] && continue
        p="$(command -v "$t" 2>/dev/null || true)"
        case "$p" in /*) ;; *) p=""; for d in /usr/bin /bin; do [ -x "$d/$t" ] && { p="$d/$t"; break; }; done ;; esac
        [ -n "$p" ] && [ -x "$p" ] && { copy_with_libs "$p" || true; }
    done
    [ -x "$DIR/bin/bash" ] || die "bash not found on host"
    ln -sf bash "$DIR/bin/sh"
    ok "host-tools rootfs created in $DIR"
else
    if [ -z "$BUSYBOX" ]; then
        BUSYBOX="$(command -v busybox 2>/dev/null || true)"
    fi
    if [ -n "$BUSYBOX" ] && ! is_static "$BUSYBOX"; then
        echo "[WARN] $BUSYBOX is dynamically linked, ignoring (need a static BusyBox)"
        BUSYBOX=""
    fi
    if [ -z "$BUSYBOX" ] && [ "$(id -u)" -eq 0 ] && command -v apt-get >/dev/null 2>&1; then
        echo "[..] trying: apt-get install -y busybox-static"
        if apt-get install -y busybox-static >/dev/null 2>&1; then
            BUSYBOX="$(command -v busybox || true)"
        fi
    fi
    if [ -z "$BUSYBOX" ]; then
        tmp="$(mktemp)"
        echo "[..] downloading $BB_URL"
        if command -v curl >/dev/null 2>&1 && curl -fsSL -o "$tmp" "$BB_URL"; then :
        elif command -v wget >/dev/null 2>&1 && wget -q -O "$tmp" "$BB_URL"; then :
        else
            rm -f "$tmp"
            die "no static BusyBox available.
  Fix (pick one):
    sudo apt update && sudo apt install busybox-static   # then re-run this script
    ./scripts/create-rootfs.sh --busybox /path/to/static/busybox
    ./scripts/create-rootfs.sh --host-tools              # offline fallback"
        fi
        chmod +x "$tmp"
        is_static "$tmp" || { rm -f "$tmp"; die "downloaded BusyBox is not static"; }
        BUSYBOX="$tmp"
    fi
    cp "$BUSYBOX" "$DIR/bin/busybox"
    chmod 0755 "$DIR/bin/busybox"
    "$DIR/bin/busybox" --list >/dev/null 2>&1 || die "BusyBox binary does not run on this machine"
    for applet in $("$DIR/bin/busybox" --list); do
        case "$applet" in
            init|linuxrc|*/*) continue ;;
        esac
        if [ -e "$DIR/bin/$applet" ]; then continue; fi
        ln -s busybox "$DIR/bin/$applet"
    done
    ok "BusyBox rootfs created in $DIR ($("$DIR/bin/busybox" 2>&1 | head -1))"
fi

# Minimal configuration files
echo "root:x:0:0:root:/root:/bin/sh"       > "$DIR/etc/passwd"
echo "nobody:x:65534:65534:nobody:/:/bin/false" >> "$DIR/etc/passwd"
echo "root:x:0:"                            > "$DIR/etc/group"
echo "nogroup:x:65534:"                    >> "$DIR/etc/group"
echo "127.0.0.1 localhost"                  > "$DIR/etc/hosts"
echo "nameserver 1.1.1.1"                   > "$DIR/etc/resolv.conf"
echo "/bin/sh"                              > "$DIR/etc/shells"
echo "export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" > "$DIR/etc/profile"
echo "PS1='boxforge:\\w# '"                >> "$DIR/etc/profile"
ok "rootfs ready: $DIR"
