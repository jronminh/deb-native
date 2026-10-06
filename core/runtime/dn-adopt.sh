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
# At launch, the same adoption happens automatically: the shim's do_exec
# hands a foreign glibc binary to dn-run, which adopts it (lazy adopt,
# docs/log/findings/lazy-adopt-foreign-binaries.md). This tool is for adopting
# ahead of time, in bulk.
#
# Usage:
#   dn-adopt.sh PREFIX FILE...          adopt the given files
#   dn-adopt.sh PREFIX --scan [DIR...]  adopt every candidate ELF under DIR
#                                       (default: ~/.local/bin)
set -eu
DN=${1:?usage: dn-adopt.sh PREFIX FILE... | dn-adopt.sh PREFIX --scan [DIR...]}
shift
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

[ -x "$LD" ] || { echo "E: no fused glibc loader in $DN (install the prefix first)" >&2; exit 1; }

# dn-elf is the overlay's editor; it writes the interpreter in place or grows
# one PT_LOAD to map a longer path, so adoption needs no patchelf.
ELF="$DN/usr/lib/deb-native/dn-elf"
[ -x "$ELF" ] || {
  echo "E: no $ELF -- adopting a glibc binary needs it (install the runtime overlay)" >&2
  exit 1
}

adopt_one() {
  f=$1
  if [ ! -f "$f" ]; then
    echo "E: $f: no such file" >&2; return 1
  fi
  if [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" != 7f454c46 ]; then
    echo "$f: not an ELF program; left alone."; return 0
  fi
  interp=$("$ELF" get-interp "$f" 2>/dev/null) || interp=""
  # NB: do NOT test the interpreter with [ -e ]. This runs inside the prefix,
  # where the shim rewrites /lib -> $DN/lib, so a missing /lib loader would
  # look present and never get adopted. A Termux glibc program is identified
  # by its interpreter living under $TP instead. (Same class of trap as the
  # shim's do_exec; docs/log/findings/lazy-adopt-foreign-binaries.md.)
  case "$interp" in
    "$LD") echo "$f: already adopted."; return 0 ;;
    "$TP"/*) echo "$f: its loader $interp is Termux's; left alone."; return 0 ;;
    */ld-linux-aarch64.so.1) ;;
    '') echo "$f: static or a library; left alone."; return 0 ;;
    *) echo "$f: interpreter $interp is not glibc's; left alone."; return 0 ;;
  esac
  if [ ! -w "$f" ]; then
    echo "E: $f: not writable" >&2; return 1
  fi

  "$ELF" set-interp "$f" "$LD"
  echo "$f: adopted."
  return 0
}

rc=0
if [ "${1:-}" = "--scan" ]; then
  shift
  [ $# -gt 0 ] || set -- "$HOME/.local/bin"
  TMP=$(mktemp)
  trap 'rm -f "$TMP"' EXIT
  for d in "$@"; do
    if [ ! -d "$d" ]; then
      echo "E: $d: not a directory" >&2; rc=1; continue
    fi
    find "$d" -type f -perm -u+x 2>/dev/null > "$TMP"
    while IFS= read -r f; do
      adopt_one "$f" || rc=1
    done < "$TMP"
  done
else
  [ $# -gt 0 ] || { echo "usage: dn-adopt.sh PREFIX FILE... | dn-adopt.sh PREFIX --scan [DIR...]" >&2; exit 2; }
  for f in "$@"; do
    adopt_one "$f" || rc=1
  done
fi
exit $rc
