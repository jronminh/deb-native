#!/bin/sh
# Trim a prefix to the minimal apt/dpkg/bash-capable set (build-stage helper,
# run before package-prefix.sh; docs/notes/modularize.md "Build vs Ship"). Keeps the
# Debian Essential/Required floor plus apt/dpkg's dependency closure plus the
# full deb-native overlay (binaries AND scripts); removes what only inflates
# size. Experimental -- measure before trusting.
#
# Usage: trim-prefix.sh PREFIX [--strip]
set -eu
DN=${1:?usage: trim-prefix.sh PREFIX [--strip]}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
[ -d "$DN" ] || { echo "E: no such prefix: $DN" >&2; exit 1; }
STRIP=0
[ "${2:-}" = "--strip" ] && STRIP=1

before=$(du -sm "$DN" 2>/dev/null | cut -f1)

# 1. Docs, man, info, locales, lintian -- pure weight, never needed at runtime.
rm -rf "$DN/usr/share/doc" "$DN/usr/share/man" "$DN/usr/share/info" \
       "$DN/usr/share/locale" "$DN/usr/share/lintian" 2>/dev/null || true

# 2. Dev-only files (runtime uses the versioned shared objects).
rm -rf "$DN/usr/include" 2>/dev/null || true
find "$DN/usr/lib" "$DN/lib" -name '*.a' -delete 2>/dev/null || true

# 3. Caches and package lists -- the user runs `apt update` after extracting.
rm -rf "$DN/var/cache/apt"/* "$DN/var/cache/debconf"/* 2>/dev/null || true
rm -rf "$DN/var/lib/apt/lists"/* 2>/dev/null || true

# 4. Optional: strip runtime ELFs (keep dynamic symbols). Off by default --
#    verify a maintainer script still runs before trusting it.
if [ "$STRIP" = 1 ] && command -v strip >/dev/null 2>&1; then
  find "$DN/usr" -type f -exec sh -c 'head -c4 "$1" | od -An -tx1 | grep -q "7f 45 4c 46" && strip "$1" 2>/dev/null || true' sh {} \; 2>/dev/null || true
fi

after=$(du -sm "$DN" 2>/dev/null | cut -f1)
echo "trimmed: $DN (${before}MB -> ${after}MB)"
echo "kept: Essential/Required + apt/dpkg deps + the deb-native overlay (binaries and scripts)"
