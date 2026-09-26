#!/bin/sh
# True fusion (naibed): repoint a just-installed package's
# own glibc ELFs at Termux's real glibc loader (the same one-time
# PT_INTERP + RUNPATH edit the classic branch's patch-elfs.sh does via
# `grun --configure`), scoped to that package's own files via `dpkg -L`.
#
# NEVER do this as a blind find over $INSTDIR the way patch-elfs.sh does:
# fusion mode's $INSTDIR IS Termux's own real, shared $PREFIX, holding
# thousands of Termux's own native Bionic binaries. `grun --configure`
# (glibc-runner.sh's `_glibc-runner_set_up_binary()`) calls `patchelf
# --set-interpreter` unconditionally on whatever file it's given -- no
# glibc/Bionic check of its own (read the source, not assumed). Handed a
# Bionic ELF, it would overwrite that binary's real, working interpreter
# too, corrupting it. Scoping to one package's own file list, and
# independently re-checking each file's own PT_INTERP for "ld-linux"
# before patching, is load-bearing here, not a style choice.
#
# `dpkg -L` prints each file's raw stored path (fuse-repack.sh already
# stripped "usr/" at package-build time, so these look like "/bin/x",
# never "$INSTDIR/bin/x") -- it is NOT joined with any root by dpkg itself
# on this build (confirmed: none of them exist as literal, unjoined
# strings). Must be joined with INSTDIR here, explicitly, before checking
# anything on disk.
#
# Usage: fuse-patch-elfs.sh INSTDIR PACKAGE
set -eu
INSTDIR=${1:?usage: fuse-patch-elfs.sh INSTDIR PACKAGE}
PKG=${2:?usage: fuse-patch-elfs.sh INSTDIR PACKAGE}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac

is_glibc_elf() {
  [ -f "$1" ] && [ -x "$1" ] || return 1
  case "$(head -c4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) ;;
    *) return 1 ;;
  esac
  readelf -p .interp "$1" 2>/dev/null | grep -q "ld-linux"
}

dpkg -L "$PKG" 2>/dev/null | while IFS= read -r f; do
  real="$INSTDIR$f"
  is_glibc_elf "$real" || continue
  grun --configure "$real" >/dev/null 2>&1 || true
done
