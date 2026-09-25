#!/bin/sh
# Generate launcher wrappers for a prefix's installed programs, so a user
# can just type `prog` instead of running it through dn-shell by hand.
#
# Why wrappers at all: an installed Debian binary is grun-repointed at
# $PREFIX/glibc/lib/ld-linux, and only with the path-redirect shim in its
# environment does it run (and then its hardcoded /usr, /etc, /var, /opt
# paths resolve into the prefix). A wrapper sets that environment and execs
# the real program.
#
# Where they go: NOT $INSTDIR/bin -- base-files' usrmerge makes that a
# symlink to usr/bin, so writing there would clobber real binaries (found
# the hard way). A dedicated dir under usr/lib/deb-native instead, which
# scripts/dn-activate.sh puts first on PATH.
#
# Usage: make-launchers.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-launchers.sh INSTDIR}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
SHIM="$INSTDIR/usr/lib/deb-native/path-redirect.so"
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"

[ -f "$SHIM" ] || { echo "make-launchers: missing $SHIM" >&2; exit 1; }
[ -x "$INSTDIR/usr/bin/dn-shell" ] || { echo "make-launchers: run setup-runtime.sh first" >&2; exit 1; }

mkdir -p "$LAUNCHDIR"
tmp="$LAUNCHDIR/.tmp.$$"

is_elf() {
  [ "$(head -c4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}

# Wrap $name -> $real. Wrapper type depends on the target: an ELF gets the
# shim environment directly; a #! script is run through the glibc shell
# wrappers (so its own hardcoded paths are covered and its interpreter
# resolves), because handing a Bionic /bin/sh a glibc LD_PRELOAD crashes.
wrap() {
  name=$1; real=$2
  case "$name" in
    dn-shell|dn-perl|chown|chgrp|path-redirect.so|'') return 0 ;;
  esac
  if is_elf "$real"; then
    cat > "$tmp" <<EOF
#!/system/bin/sh
export DN_INSTDIR="$INSTDIR"
export PATH="$INSTDIR/usr/sbin:$INSTDIR/usr/bin:$INSTDIR/sbin:$INSTDIR/bin:$LAUNCHDIR:$GLIBC/bin:$PREFIX_DIR/bin"
# Hand Termux's own preload (termux-exec) back to any Bionic child via the
# shim's DN_BIONIC_PRELOAD, instead of dropping it -- Termux-native
# commands need it, and it must not be disabled system-wide.
case "\$LD_PRELOAD" in
  *path-redirect.so*) ;;
  *) [ -n "\$LD_PRELOAD" ] && export DN_BIONIC_PRELOAD="\$LD_PRELOAD" ;;
esac
export LD_PRELOAD="$SHIM"
exec "$real" "\$@"
EOF
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
for d in "$INSTDIR/usr/bin" "$INSTDIR/usr/sbin" "$INSTDIR/sbin" "$INSTDIR/bin"; do
  [ -d "$d" ] || continue
  [ -L "$d" ] && continue
  for f in "$d"/*; do
    [ -e "$f" ] || continue
    [ -L "$f" ] && continue
    [ -x "$f" ] || continue
    [ -f "$f" ] || continue
    wrap "$(basename "$f")" "$f"
  done
  # Also expose a program's alternative name if a <name>-<pkg> provider
  # exists next to a dangling alternatives symlink. update-alternatives on
  # this device writes those links into Termux's own prefix (a real bug,
  # noted in TODO.md), so the links themselves cannot be followed.
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

rm -f "$tmp"
echo "==> launchers in $LAUNCHDIR ($(ls -1 "$LAUNCHDIR" | wc -l) programs)"
