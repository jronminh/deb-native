#!/bin/sh
# Phase: apt's DPkg::Post-Invoke hook (MODULARIZE.md, "Post-build pipelines").
# Runs once after each transaction, with the prefix's own files installed, in
# this order:
#   1. glibc: restore the patched glibc files (dn-fix-glibc),
#   2. alternatives: relative update-alternatives links (dn-fix-alternatives.sh),
#   3. symlinks: absolute links into the prefix made relative (normalize),
#   4. launchers for the prefix's programs (make-launchers.sh),
#   5. gcc specs: gcc's default loader points at this prefix (gcc-specs).
# Never fails the transaction: every step logs, the hook exits 0.
#
# Usage: dn-hook-post.sh [PREFIX]   (apt passes none; the prefix is found from
# this script's location)
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The prefix is where this hook lives: $PREFIX/usr/lib/deb-native/scripts/install.
# An explicit PREFIX argument still wins (the in-place bootstrap passes one).
if [ -n "${1:-}" ] && [ -d "$1/usr/lib/deb-native" ]; then DN=$1; else DN=$(CDPATH= cd -- "$HERE/../../../../.." && pwd); fi
LOG="$DN/var/log/deb-native-hook.log"
mkdir -p "$DN/var/log"

# 1. The patched glibc files, restored from the stash when apt put stock ones back.
fix_glibc() {
  STASH="$DN/usr/lib/deb-native/glibc-swap"
  [ -d "$STASH" ] || return 0
  fixed=0
  for rel in \
    usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
    usr/lib/aarch64-linux-gnu/libc.so.6 \
    usr/lib/aarch64-linux-gnu/libnsl.so.1 \
    usr/lib/aarch64-linux-gnu/libnss_compat.so.2 \
    usr/lib/aarch64-linux-gnu/libnss_hesiod.so.2 \
    usr/lib/aarch64-linux-gnu/libresolv.so.2 \
    usr/lib/aarch64-linux-gnu/librt.so.1 \
    usr/sbin/ldconfig \
    usr/bin/iconv \
    usr/bin/localedef; do
    src="$STASH/$rel"; dst="$DN/$rel"
    [ -e "$src" ] || continue
    if [ -e "$dst" ] && cmp -s "$src" "$dst"; then continue; fi
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst.tmp.$$" && chmod 755 "$dst.tmp.$$" && mv -f "$dst.tmp.$$" "$dst"
    fixed=$((fixed + 1))
  done
  [ "$fixed" -gt 0 ] && echo "dn-fix-glibc: restored $fixed patched glibc file(s)"
  return 0
}

# 3. Absolute symlinks that point into the prefix become relative, so the kernel
#    resolves them inside the prefix. Repeats until nothing changes (5 passes).
normalize_symlinks() {
  ROOT=$DN
  BOUND="usr etc var opt bin sbin tmp run"
  [ -d "$ROOT" ] || return 0
  relpath() {
    "$AWK" -v from="$1" -v to="$2" 'BEGIN {
      nf = split(from, a, "/"); nt = split(to, b, "/"); i = 1
      while (i <= nf && i <= nt && a[i] == b[i]) i++
      out = ""
      for (j = i; j <= nf; j++) out = out (out == "" ? "" : "/") ".."
      for (j = i; j <= nt; j++) out = out (out == "" ? "" : "/") b[j]
      if (out == "") out = "."
      print out }'
  }
  tmp=$(mktemp) || return 0
  pass=0; changed=1
  while [ "$changed" -eq 1 ] && [ "$pass" -lt 5 ]; do
    pass=$((pass + 1)); changed=0
    find "$ROOT" -xdev -type l > "$tmp" || true
    while IFS= read -r link; do
      target=$(readlink "$link") || continue
      case "$target" in
      /*)
        # Only absolute targets that land in a bound directory can escape.
        rest=${target#/}; first=${rest%%/*}
        case " $BOUND " in *" $first "*) ;; *) continue ;; esac
        rel=$(relpath "$(dirname "$link")" "$ROOT$target")
        [ "$rel" = "$target" ] && continue
        ln -sfn "$rel" "$link"; changed=1 ;;
      *)
        # A relative target that does not resolve, computed against a logical
        # directory (merged /usr): if its logical meaning lands on a real file,
        # rewrite the link relative to its physical directory.
        [ -e "$link" ] && continue
        case "$link" in "$ROOT/usr/bin/"*|"$ROOT/usr/sbin/"*) ;; *) continue ;; esac
        case "$target" in ../*) ;; *) continue ;; esac
        cand="$ROOT/${target#../}"
        [ -e "$cand" ] || continue
        ln -sfn "$(relpath "$(dirname "$link")" "$cand")" "$link"; changed=1 ;;
      esac
    done < "$tmp"
  done
  rm -f "$tmp"
  echo "Normalized symlinks in $ROOT ($pass pass(es))."
  return 0
}

# 5. gcc's default dynamic linker -> this prefix's loader (no-op without gcc).
fix_gcc_specs() {
  INTERP="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
  [ -x "$INTERP" ] || return 0
  [ -d "$DN/usr/lib/gcc" ] || return 0
  OLD='/lib/ld-linux-aarch64%{mbig-endian:_be}%{mabi=ilp32:_ilp32}.so.1'
  for d in "$DN"/usr/lib/gcc/*/*; do
    [ -d "$d" ] || continue
    target=$(basename "$(dirname "$d")"); ver=$(basename "$d")
    specs="$d/specs"
    [ -f "$specs" ] && grep -q "$INTERP" "$specs" 2>/dev/null && continue
    gcc_bin="$DN/usr/bin/$target-gcc-$ver"
    [ -x "$gcc_bin" ] || gcc_bin="$DN/usr/bin/$target-gcc"
    [ -x "$gcc_bin" ] || continue
    "$gcc_bin" -dumpspecs 2>/dev/null | sed "s#$OLD#$INTERP#" > "$specs.new" \
      && mv "$specs.new" "$specs" \
      || rm -f "$specs.new"
  done
  return 0
}

AWK="$DN/usr/bin/mawk"
echo "== $(date '+%F %T') post" >> "$LOG"
{
  fix_glibc
  "$HERE/dn-fix-alternatives.sh" "$DN"
  normalize_symlinks
  "$HERE/../runtime/make-launchers.sh" "$DN"
  fix_gcc_specs
} 2>&1 | tee -a "$LOG"
exit 0
