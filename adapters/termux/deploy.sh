#!/bin/sh
# Phase A -- Termux target: deploy a shipped prefix image.
#
# Extract the image into PREFIX, relocate it to PREFIX (re-point every ELF's
# interpreter at the prefix loader, then rewrite baked paths in the text
# config), then run the prefix's own Phase B finish. Self-contained on purpose:
# it runs before the prefix exists, so it must not depend on the repo or on the
# prefix itself. Needs tar, patchelf and sed on the host.
#
# If the image was already built for PREFIX (baked), pass OLD_PREFIX=PREFIX --
# leg 2 is then skipped, and leg 1 is a no-op.
#
# Usage: deploy.sh IMAGE PREFIX [OLD_PREFIX]
set -eu
IMG=${1:?usage: deploy.sh IMAGE PREFIX [OLD_PREFIX]}
DN=${2:?usage: deploy.sh IMAGE PREFIX [OLD_PREFIX]}
OLD=${3:-}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
[ -f "$IMG" ] || { echo "E: no such image: $IMG" >&2; exit 1; }

mkdir -p "$DN"
tar xzf "$IMG" -C "$DN"

# Leg 1 -- ELF interpreters.
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
find "$DN" -type f 2>/dev/null | while IFS= read -r f; do
  i=$(patchelf --print-interpreter "$f" 2>/dev/null) || continue
  [ -n "$i" ] || continue
  [ "$i" = "$LD" ] && continue
  patchelf --set-interpreter "$LD" "$f" 2>/dev/null || true
done

# Leg 2 -- baked path strings in the text config.
if [ -n "$OLD" ] && [ "$OLD" != "$DN" ]; then
  for f in "$DN/etc/ld.so.preload" "$DN/etc/apt.conf" "$DN/etc/apt/apt.conf" \
           "$DN/etc/apt/sources.list" "$DN/usr/etc/ld.so.conf"; do
    [ -f "$f" ] || continue
    grep -qF "$OLD" "$f" 2>/dev/null && sed -i "s|$OLD|$DN|g" "$f" || true
  done
fi

# Phase B -- finish inside the prefix, using its own shell.
FIN=/usr/lib/deb-native/scripts/runtime/dn-finish.sh
for sh in "$DN/usr/bin/bashell" "$DN/usr/bin/bash" "$DN/usr/bin/bash"; do
  [ -x "$sh" ] || continue
  "$sh" -c "sh $FIN $DN" && break || true
done

echo "deploy: $DN ready."
