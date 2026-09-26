#!/bin/sh
# Repoint every Debian glibc ELF in the prefix at Termux's glibc loader
# (`grun --configure`: a one-time PT_INTERP + RUNPATH edit), so the binaries
# actually run. This has to happen after dpkg installs/unpacks them, so it
# is one of the pipeline steps both apt-install.sh and the apt Post-Invoke
# hook run.
#
# Never touches this project's own runtime: dn-shell/dn-perl are Bionic
# launchers and the shim is a glibc .so; rewriting their interpreter breaks
# them ("libdl.so: cannot open shared object file").
#
# Usage: patch-elfs.sh ROOT
set -eu
ROOT=${1:?usage: patch-elfs.sh ROOT}
case "$ROOT" in /*) ;; *) ROOT="$PWD/$ROOT" ;; esac

find "$ROOT" -type f -perm -u+x 2>/dev/null |
  while IFS= read -r f; do
    case "$f" in
      "$ROOT/lib/deb-native/"*|\
      "$ROOT/usr/lib/deb-native/"*|\
      "$ROOT/usr/bin/dn-shell"|\
      "$ROOT/usr/bin/dn-perl") continue ;;
    esac
    case "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
      7f454c46) grun --configure "$f" >/dev/null 2>&1 || true ;;
    esac
  done
