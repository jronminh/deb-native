#!/bin/sh
# Scan ELF files for the dynamic libc symbols they import, to find which
# path-taking entry points the shim actually needs to intercept.
#
# Usage:
#   scan-libc-symbols.sh DIR          # scan an extracted tree (e.g. an instdir)
#   scan-libc-symbols.sh --debs DIR   # scan .deb files under DIR (to a temp root)
#
# Prints, on stdout: "<count> <symbol>" per undefined dynamic symbol, plus
# NOSTATIC/NODYNSYM markers so static binaries and raw-syscall users are visible.
set -eu
mode=tree
if [ "${1:-}" = "--debs" ]; then mode=debs; shift; fi
target=${1:?usage: $0 [--debs] DIR}

tmp=
cleanup() { [ -n "$tmp" ] && rm -rf "$tmp"; }
trap cleanup EXIT

if [ "$mode" = debs ]; then
  tmp=$(mktemp -d)
  find "$target" -name '*.deb' | while IFS= read -r d; do
    dpkg-deb -x "$d" "$tmp/x" 2>/dev/null || continue
  done
  target=$tmp/x
fi

find "$target" -type f 2>/dev/null | while IFS= read -r f; do
  [ "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || continue
  if ! readelf -d "$f" >/dev/null 2>&1; then
    echo "1 STATIC_ELF"
    continue
  fi
  if readelf -lW "$f" 2>/dev/null | grep -q 'INTERP'; then :; else
    echo "1 NO_INTERP"
  fi
  nm -D -u "$f" 2>/dev/null | awk 'NF{print $NF}' | sed 's/@.*//'
done | sort | uniq -c | sort -rn
