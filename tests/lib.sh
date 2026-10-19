# lib.sh - tiny test helpers (sourced)
PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  [PASS] $*"; }
fail() { FAIL=$((FAIL+1)); echo "  [FAIL] $*"; }
skip() { SKIP=$((SKIP+1)); echo "  [SKIP] $*"; }
section() { echo; echo "== $*"; }
ctl_available() { # ctl_available cpu|memory
    local root; root="$(awk '$3=="cgroup2"{print $2; exit}' /proc/mounts)"
    [ -n "$root" ] && grep -qw "$1" "$root/cgroup.controllers" 2>/dev/null
}
