#!/bin/sh
# Debian mode index translation (docs/debian-mode.md): rewrite
# "Architecture: all" to "arm64" in every Debian binary-arm64 Packages list,
# so Debian's arch-independent packages live on the arm64 side and resolve
# their dependencies against Debian, never against Termux's same-named "all"
# packages (dpkg treats "all" as the native architecture, which is Termux's).
# Run by apt after each successful update; idempotent.
#
# Usage: dn-debian-index.sh LISTS_DIR
set -eu
LISTS=${1:?usage: dn-debian-index.sh LISTS_DIR}
for f in "$LISTS"/*_binary-arm64_Packages; do
  [ -f "$f" ] || continue
  grep -q '^Architecture: all$' "$f" || continue
  sed -i 's/^Architecture: all$/Architecture: arm64/' "$f"
done
