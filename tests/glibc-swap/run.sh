#!/bin/sh
# On-device acceptance test for a prefix already deployed by the swap
# (0.6.0+s.1, s = swap): Debian's real libc6/libc-bin, then this project's
# own-built, Android-patched glibc (fused loader + the 10 files) and the
# path shim via etc/ld.so.preload -- no ld-dn trampoline. This script does
# NOT bootstrap and does NOT download: point it at a prefix that is already
# deployed. See docs/spec/dn-glibc-prefix.md.
#
# Usage: run.sh PREFIX [fresh|live]
#   fresh  a prefix from a fresh install of this version (default). Checks
#          the whole swap: libc6 AND libc-bin installed and held, the loader
#          is this project's own (__dn_prefix_get), every PT_INTERP is the
#          fused loader, shim preloaded, cache built, runs, NSS.
#   live   an existing prefix upgraded in place. The same functional checks,
#          but tolerant of a not-yet-migrated package set (libc-bin may not
#          be registered, the loader may be an earlier own build).
#
# Env: DN_GLIBC_DEBS  a bundle dir; if set, cmp the swapped files against
#      its files/ (fresh mode).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
REL_INTERP=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
TERMUX_PREFIX=${DN_HOST_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

fail() { printf 'FAIL: glibc-swap: %s\n' "$1" >&2; exit 1; }
info() { printf '  %s\n' "$1"; }

P=${1:?usage: run.sh PREFIX [fresh|live]}
MODE=${2:-fresh}
case "$P" in /*) ;; *) P="$PWD/$P" ;; esac
case "$MODE" in fresh|live) ;; *) fail "unknown mode '$MODE' (fresh|live)";; esac

[ -s "$P/var/lib/dpkg/status" ] || fail "not a bootstrapped prefix: $P"

# --- 1. Debian package identity -----------------------------------------
Q="$P/usr/bin/dpkg-query"
q() { "$Q" --admindir="$P/var/lib/dpkg" -W "$1" 2>/dev/null; }
v6=$(q libc6)
case "$v6" in *libc6:arm64*) ;; *) fail "libc6 not installed ('$v6')";; esac
info "libc6: $v6"

# --- 2. The swap: our loader is in place --------------------------------
LD="$P/$REL_INTERP"
[ -e "$LD" ] || fail "fused loader missing: $LD"
if nm -D "$LD" 2>/dev/null | grep -q '__dn_prefix_get'; then
  info "loader is this project's build (__dn_prefix_get exported)"
elif [ "$MODE" = live ]; then
  info "loader is an earlier own build (no __dn_prefix_get) -- ok in live mode"
else
  fail "loader is not this project's build (no __dn_prefix_get): $LD"
fi

# Every ELF with an interpreter points at the prefix's own loader -- no ld-dn.
bad=0
for f in "$P/usr/bin/"* "$P/usr/sbin/"*; do
  [ -f "$f" ] || continue
  interp=$(readelf -l "$f" 2>/dev/null | grep -o 'interpreter: [^]]*' | sed 's/interpreter: //') || true
  [ -n "$interp" ] || continue
  case "$interp" in
    *ld-dn*) printf '    ld-dn still the interpreter: %s\n' "$f"; bad=1 ;;
    "$LD") ;;
    */system/bin/linker*|*linker64*) ;; # Bionic launcher (dn-shell, dn-perl): intentional
    *) printf '    wrong interpreter: %s -> %s\n' "$f" "$interp"; bad=1 ;;
  esac
done
[ "$bad" = 0 ] || fail "not every program uses the fused loader"
info "every binary's interpreter is the prefix's own ld-linux (no ld-dn)"

# --- 3. The shim is preloaded, and the cache is built --------------------
PRE=$P/etc/ld.so.preload
[ -f "$PRE" ] || fail "no $PRE"
shim=$(head -n1 "$PRE")
[ "$shim" = "$P/usr/lib/deb-native/dn-shim.so" ] \
  || fail "ld.so.preload points elsewhere: '$shim'"
[ -x "$shim" ] || fail "shim not executable: $shim"
[ -s "$P/usr/etc/ld.so.cache" ] || fail "ld.so.cache missing/empty (ldconfig did not run)"
info "shim preloaded; ld.so.cache present"

# --- 4. fresh only: full package set, held, exact build ------------------
if [ "$MODE" = fresh ]; then
  vb=$(q libc-bin)
  case "$vb" in *libc-bin:arm64*) ;; *) fail "libc-bin not installed ('$vb')";; esac
  info "libc-bin: $vb"
  for p in libc6 libc-bin; do
    "$TERMUX_PREFIX/bin/dpkg" --admindir="$P/var/lib/dpkg" --get-selections 2>/dev/null \
      | grep -E "^$p(:arm64)?[[:space:]]+hold" >/dev/null \
      || fail "$p is not held (apt upgrade could replace it)"
  done
  info "libc6/libc-bin are held"

  if [ -n "${DN_GLIBC_DEBS:-}" ]; then
    case "$DN_GLIBC_DEBS" in /*) ;; *) DN_GLIBC_DEBS="$PWD/$DN_GLIBC_DEBS" ;; esac
    for rel in \
      usr/lib/aarch64-linux-gnu/libc.so.6 \
      usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
      usr/sbin/ldconfig ; do
        cmp -s "$P/$rel" "$DN_GLIBC_DEBS/files/$rel" \
          || fail "not the bundle's build: $rel"
    done
    info "swapped files match the bundle byte-for-byte"
  fi
fi

# --- 5. Usable (no downloads) -------------------------------------------
R=$P/usr/lib/deb-native/dn-run
[ -x "$R" ] || fail "no dn-run (run setup-runtime.sh)"
"$R" "$P/usr/bin/ls" -1 "$P/usr" >/dev/null 2>&1 || fail "ls did not run"
info "a base program runs"

idout=$("$R" "$P/usr/bin/id" 2>&1) || fail "id failed: $idout"
case "$idout" in *"uid=0(root)"*) ;; *) fail "fake root missing: $idout";; esac
info "fake root: $idout"

ent=$("$R" "$P/usr/bin/getent" passwd root 2>&1) || fail "getent failed: $ent"
case "$ent" in root:*) ;; *) fail "NSS did not resolve root: $ent";; esac
info "NSS resolves the prefix's /etc: $ent"

echo "PASS: glibc-swap [$MODE] ($P)"
