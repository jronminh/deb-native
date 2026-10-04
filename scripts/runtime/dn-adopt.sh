#!/bin/sh
# Adopt a glibc arm64 program obtained outside apt (a direct installer, a
# release tarball, a downloaded binary) into the prefix: the same ELF step
# dn-translate-deb.sh applies to every .deb, on one file, in place.
#
#   - its interpreter becomes the prefix's own fused glibc loader
#     ($DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1), so the kernel
#     runs that loader first however the program is started: it derives the
#     prefix from its own path, reads the path shim from
#     $DN/etc/ld.so.preload and the prefix's libraries from
#     $DN/usr/etc/ld.so.cache, and /proc/self/exe stays the program
#     (single-file builds that read themselves -- Bun, Node SEA, Claude
#     Code's native binary -- keep working). No RUNPATH rewrite: the
#     loader's ld.so.cache already covers the prefix's library dirs for
#     every adopted program, and patching RUNPATH risked corrupting the
#     program headers of a tightly-packed ET_EXEC binary (found on gcc's
#     cc1, docs/log/findings/patchelf-et-exec-runpath.md).
#
# Only a program whose interpreter is a glibc loader that does not exist on
# this device (/lib/ld-linux-aarch64.so.1) is adopted; Termux's own glibc
# programs, Bionic programs, static programs and scripts are left alone.
# Idempotent. Self-updating programs download a fresh, unadopted binary on
# update: adopt that one again, or turn their updater off.
#
# Usage: dn-adopt.sh PREFIX FILE...
set -eu
DN=${1:?usage: dn-adopt.sh PREFIX FILE...}
shift
[ $# -gt 0 ] || { echo "usage: dn-adopt FILE..." >&2; exit 2; }
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

[ -x "$LD" ] || { echo "E: no fused glibc loader in $DN (install the prefix first)" >&2; exit 1; }

rc=0
for f in "$@"; do
  if [ ! -f "$f" ]; then
    echo "E: $f: no such file" >&2; rc=1; continue
  fi
  if [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" != 7f454c46 ]; then
    echo "$f: not an ELF program; left alone."; continue
  fi
  interp=$(patchelf --print-interpreter "$f" 2>&1) || interp=""
  case "$interp" in
    "$LD") echo "$f: already adopted."; continue ;;
    */ld-linux-aarch64.so.1) ;;
    '') echo "$f: static or a library; left alone."; continue ;;
    *) echo "$f: interpreter $interp is not glibc's; left alone."; continue ;;
  esac
  if [ -e "$interp" ]; then
    echo "$f: its loader $interp exists here (a Termux glibc program); left alone."
    continue
  fi
  [ -w "$f" ] || { echo "E: $f: not writable" >&2; rc=1; continue; }

  patchelf --set-interpreter "$LD" "$f"
  echo "$f: adopted."
done
exit $rc
