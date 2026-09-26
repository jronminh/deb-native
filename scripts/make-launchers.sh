#!/bin/sh
# Generate launcher wrappers for a prefix's installed programs, so a user
# can just type `prog` instead of running it through dn-shell by hand.
#
# Why wrappers at all: an installed Debian ELF is grun-repointed at
# $PREFIX/glibc/lib/ld-linux, and needs the right launch mechanism for its
# hardcoded /usr, /etc, /var, /opt paths to resolve into the prefix. The
# wrapper hands the real binary to dn-run, which classifies it at launch:
# glibc -> LD_PRELOAD the path-redirect shim; static, NSS, and direct-syscall
# binaries -> the syscall tracer; Bionic -> plain exec. Scripts still get the
# glibc shell/perl wrappers below.
#
# Where they go: NOT $INSTDIR/bin -- base-files' usrmerge makes that a
# symlink to usr/bin, so writing there would clobber real binaries (found
# the hard way). A dedicated dir under usr/lib/deb-native instead, which
# scripts/dn-activate.sh puts first on PATH.
#
# Usage: make-launchers.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-launchers.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
SHIM="$INSTDIR/usr/lib/deb-native/path-redirect.so"
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"

[ -f "$SHIM" ] || { echo "make-launchers: missing $SHIM" >&2; exit 1; }
[ -x "$INSTDIR/usr/lib/deb-native/dn-run" ] || { echo "make-launchers: run setup-runtime.sh first (no dn-run)" >&2; exit 1; }
[ -x "$INSTDIR/usr/bin/dn-shell" ] || { echo "make-launchers: run setup-runtime.sh first" >&2; exit 1; }

mkdir -p "$LAUNCHDIR"
tmp="$LAUNCHDIR/.tmp.$$"
BIN_DIRS="$INSTDIR/usr/bin $INSTDIR/usr/sbin $INSTDIR/sbin $INSTDIR/bin $INSTDIR/usr/games"

# Programs whose own code issues syscalls (inline `svc #0`) or imports
# `syscall()`; the shim cannot see those, so force the tracer. Computed once
# here because disassembling per launch would be far too slow. See
# docs/syscall-boundary.md, "Remaining: the direct-syscall attribute".
DIRECT_LIST="$tmp.direct"
: > "$DIRECT_LIST"
if command -v python3 >/dev/null 2>&1; then
  for d in $BIN_DIRS; do
    [ -d "$d" ] || continue
    [ -L "$d" ] && continue
    python3 "$HERE/scan-direct-syscalls.py" "$d" --trace-list >> "$DIRECT_LIST" 2>/dev/null || true
  done
fi

is_elf() {
  [ "$(head -c4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}

# Wrap $name -> $real. Wrapper type depends on the target: an ELF is handed
# to dn-run for launch-time classification; a #! script is run through the
# glibc shell wrappers (so its own hardcoded paths are covered and its
# interpreter resolves), because handing a Bionic /bin/sh a glibc LD_PRELOAD
# crashes.
wrap() {
  name=$1; real=$2
  case "$name" in
    dn-shell|dn-perl|chown|chgrp|path-redirect.so|'') return 0 ;;
  esac
  if is_elf "$real"; then
    # dn-run classifies at launch: shim (glibc), tracer (static/NSS), or a
    # plain exec (Bionic). A binary with its own syscalls is tagged --trace so
    # it skips the classifier's shim route. See native/dn-run.c.
    if grep -qxF "$real" "$DIRECT_LIST"; then
      cat > "$tmp" <<EOF
#!/system/bin/sh
exec "$LAUNCHDIR/../dn-run" --trace "$real" "\$@"
EOF
    else
      cat > "$tmp" <<EOF
#!/system/bin/sh
exec "$LAUNCHDIR/../dn-run" "$real" "\$@"
EOF
    fi
  else
    first=$(head -c 64 "$real" 2>/dev/null | head -1)
    case "$first" in
      '#!'*perl*) interp="$INSTDIR/usr/bin/dn-perl" ;;
      '#!'*)      interp="$INSTDIR/usr/bin/dn-shell" ;;
      *)          return 0 ;;
    esac
    cat > "$tmp" <<EOF
#!/system/bin/sh
exec "$interp" "$real" "\$@"
EOF
  fi
  chmod 755 "$tmp"
  mv -f "$tmp" "$LAUNCHDIR/$name"
}

# Real bin dirs (skip the usrmerge symlinks: bin -> usr/bin, sbin -> usr/sbin).
for d in $BIN_DIRS; do
  [ -d "$d" ] || continue
  [ -L "$d" ] && continue
  for f in "$d"/*; do
    [ -e "$f" ] || continue
    [ -x "$f" ] || continue
    [ -f "$f" ] || continue
    wrap "$(basename "$f")" "$f"
  done
  # Also expose a program's alternative name if a <name>-<pkg> provider
  # exists next to a STILL-dangling alternatives symlink (setup-runtime.sh's
  # update-alternatives wrapper fixes the common case; this remains a
  # fallback for whatever isn't covered yet, e.g. awk -> mawk, TODO.md).
  # A working symlink is already picked up by the loop above (-f/-x follow
  # it), so this only ever fires for one that still doesn't resolve.
  for f in "$d"/*; do
    [ -L "$f" ] || continue
    name=$(basename "$f")
    [ -e "$f" ] && continue
    provider=
    for cand in "$d/$name-"*; do
      [ -e "$cand" ] || continue
      provider=$cand
      break
    done
    [ -n "$provider" ] && wrap "$name" "$provider"
  done
done

rm -f "$tmp" "$DIRECT_LIST"
echo "==> launchers in $LAUNCHDIR ($(ls -1 "$LAUNCHDIR" | wc -l) programs)"
