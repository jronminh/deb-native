#!/bin/sh
# Relocate a prefix to a new path (Phase A of ship; MODULARIZE.md). Two legs:
#   1. re-point every ELF's PT_INTERP at the new prefix's loader (patchelf);
#   2. rewrite baked absolute paths in text files -- ld.so.preload, the apt
#      config, ld.so.conf / ld.so.conf.d -- from the old prefix path to the new.
# Idempotent. patchelf + sed must be reachable target-side, before the prefix
# itself can run (a prefix built for a fixed path needs no relocation at all).
#
# Usage: relocate-prefix.sh PREFIX NEW_PREFIX [OLD_PREFIX]
set -eu
DN=${1:?usage: relocate-prefix.sh PREFIX NEW_PREFIX [OLD_PREFIX]}
NEW=${2:?usage: relocate-prefix.sh PREFIX NEW_PREFIX [OLD_PREFIX]}
OLD=${3:-}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
case "$NEW" in /*) ;; *) NEW="$PWD/$NEW" ;; esac
[ -d "$DN" ] || { echo "E: no such prefix: $DN" >&2; exit 1; }
command -v patchelf >/dev/null 2>&1 || { echo "E: patchelf not found (needed to relocate)" >&2; exit 1; }

LD="$NEW/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

# Leg 1: ELF interpreters (executables and the few .so that carry PT_INTERP).
find "$DN" -type f 2>/dev/null | while IFS= read -r f; do
  i=$(patchelf --print-interpreter "$f" 2>/dev/null) || continue
  [ -n "$i" ] || continue
  [ "$i" = "$LD" ] && continue
  patchelf --set-interpreter "$LD" "$f" 2>/dev/null && echo "$f" || true
done > "$DN/.relocate-elfs"
echo "relocate: $(wc -l < "$DN/.relocate-elfs" | tr -d ' ') ELFs re-pointed"
rm -f "$DN/.relocate-elfs"

# Leg 2: baked path strings in text files.
if [ -n "$OLD" ] && [ "$OLD" != "$NEW" ]; then
  set -- "$DN/etc/ld.so.preload" "$DN/etc/apt.conf" "$DN/etc/apt/apt.conf" \
         "$DN/etc/apt/sources.list" "$DN/usr/etc/ld.so.conf"
  for d in "$DN/etc/apt/sources.list.d" "$DN/usr/etc/ld.so.conf.d"; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do [ -f "$f" ] && set -- "$@" "$f"; done
  done
  for f in "$@"; do
    [ -f "$f" ] || continue
    grep -qF "$OLD" "$f" 2>/dev/null && sed -i "s|$OLD|$NEW|g" "$f" || true
  done
fi

echo "relocate: $DN -> $NEW done"
