#!/bin/sh
# Expose a prefix's installed programs by name: one entry per program in
# $INSTDIR/usr/lib/deb-native/bin, which dn-launch.c puts first on the
# userland PATH (after priv/).
#
# A Debian program sets itself up however it is started: its interpreter is
# the prefix's own fused glibc loader (dn-translate-deb.sh), and a program
# script's "#!" line points into the prefix. So most entries are plain
# symlinks -- no wrapper, no extra process. A wrapper is kept only where the
# program itself cannot do it:
#   - static binaries and programs making their own syscalls: no loader to
#     set anything up, the shim cannot see them -> dn-run --trace (tracer);
#   - a glibc program on another loader, or a script with an untranslated
#     "#!" -> dn-run / dn-shell / dn-perl.
#
# Not exposed: the prefix's base system (setup-apt-prefix.sh) and the
# stand-ins' files -- their ls, sed, grep, awk, which ... are there for
# maintainer scripts and must not shadow Termux's own in the user's shell.
# Alternatives links count as the program they resolve to (awk -> mawk).
#
# Where: NOT $INSTDIR/bin -- base-files makes that a symlink to usr/bin.
# Regenerated from scratch each run; termux-apt, termux-dpkg,
# termux-dn-doctor, dn-shell and dn-adopt (make-apt-wrappers.sh) are left
# alone.
#
# Usage: make-launchers.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-launchers.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
LIBDIR="$INSTDIR/usr/lib/deb-native"
LAUNCHDIR="$LIBDIR/bin"
LD="$INSTDIR/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

[ -x "$LD" ] || { echo "E: no fused glibc loader (install the prefix first)" >&2; exit 1; }
[ -x "$LIBDIR/dn-run" ] || { echo "E: no dn-run (run setup-runtime.sh)" >&2; exit 1; }
[ -x "$INSTDIR/usr/bin/dn-shell" ] || { echo "E: no dn-shell (run setup-runtime.sh)" >&2; exit 1; }

mkdir -p "$LAUNCHDIR"
tmp="$LAUNCHDIR/.tmp.$$"
BIN_DIRS="$INSTDIR/usr/bin $INSTDIR/usr/sbin $INSTDIR/usr/games"

# Start over: drop every entry this script made (symlinks, and 0.1.x or
# fallback wrappers), keep the termux-* commands.
for e in "$LAUNCHDIR"/* ; do
  [ -e "$e" ] || [ -L "$e" ] || continue
  case "${e##*/}" in termux-*|dn-shell|dn-adopt) continue ;; esac
  if [ -L "$e" ] || grep -q 'dn-run\|dn-shell\|dn-perl' "$e"; then rm -f "$e"; fi
done

# Programs whose own code issues syscalls (inline `svc #0`) or imports
# `syscall()`; the shim cannot see those, so force the tracer. See
# docs/spec/syscall-boundary.md, "Remaining: the direct-syscall attribute".
DIRECT_LIST="$tmp.direct"
: > "$DIRECT_LIST"
if [ -n "$(command -v python3 || true)" ]; then
  for d in $BIN_DIRS; do
    [ -d "$d" ] || continue
    python3 "$HERE/../bench/scan-direct-syscalls.py" "$d" --trace-list >> "$DIRECT_LIST" || true
  done
fi

# Files of the base and of the stand-ins (dpkg-maintscript-helper, ...).
BASE_FILES="$tmp.base"
{
  [ -s "$INSTDIR/var/lib/deb-native/base-packages" ] && cat "$INSTDIR/var/lib/deb-native/base-packages"
  echo libc6; echo dpkg; echo apt
} | while IFS= read -r p; do
  "$PREFIX_DIR/bin/dpkg-query" --admindir="$INSTDIR/var/lib/dpkg" -L "$p:arm64" || true
done | sed "s|^/bin/|/usr/bin/|; s|^/sbin/|/usr/sbin/|; s|^|$INSTDIR|" > "$BASE_FILES"

is_elf() {
  [ "$(head -c4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}
wrapper() {  # NAME COMMAND...  (a /system/bin/sh wrapper, 0.1.x style)
  n=$1; shift
  printf '#!/system/bin/sh\nexec %s "$@"\n' "$*" > "$tmp"
  chmod 755 "$tmp"
  mv -f "$tmp" "$LAUNCHDIR/$n"
}

# Expose $name, found at $f (maybe an alternatives link) in a bin dir.
expose() {
  name=$1; f=$2
  case "$name" in
    dn-shell|dn-perl|chown|chgrp|''|termux-*) return 0 ;;
    # The prefix's own apt/dpkg are on PATH in usr/bin; not shadowed here.
    apt|apt-get|apt-cache|apt-mark|apt-config|dpkg|dpkg-query|dpkg-deb|dpkg-split) return 0 ;;
  esac
  real=$(readlink -f "$f") || return 0
  [ -f "$real" ] || return 0
  grep -qxF "$real" "$BASE_FILES" && return 0
  if is_elf "$real"; then
    interp=$(patchelf --print-interpreter "$real" 2>&1) || interp=""
    if grep -qxF "$real" "$DIRECT_LIST"; then
      wrapper "$name" "\"$LIBDIR/dn-run\" --trace \"$f\""
    elif [ "$interp" = "$LD" ]; then
      ln -sfn "$f" "$LAUNCHDIR/$name"
    else
      wrapper "$name" "\"$LIBDIR/dn-run\" \"$f\""
    fi
  else
    first=$(head -n1 "$real")
    case "$first" in
      "#!$INSTDIR/"*) ln -sfn "$f" "$LAUNCHDIR/$name" ;;
      '#!'*perl*)     wrapper "$name" "\"$INSTDIR/usr/bin/dn-perl\" \"$f\"" ;;
      '#!'*)          wrapper "$name" "\"$INSTDIR/usr/bin/dn-shell\" \"$f\"" ;;
      *)              return 0 ;;
    esac
  fi
}

for d in $BIN_DIRS; do
  [ -d "$d" ] || continue
  for f in "$d"/*; do
    [ -e "$f" ] || continue
    [ -x "$f" ] || continue
    expose "$(basename "$f")" "$f"
  done
done

rm -f "$tmp" "$DIRECT_LIST" "$BASE_FILES"
echo "Updated launchers in $LAUNCHDIR ($(ls -1 "$LAUNCHDIR" | wc -l) entries)."
