#!/bin/sh
# Index translation (docs/spec/design.md; from the naibed branch): rewrite
# "Architecture: all" to "arm64" in every Debian binary-arm64 Packages list,
# so Debian's arch-independent packages live on the arm64 side and resolve
# their dependencies against Debian. dpkg treats "all" as the native
# architecture, which for Termux's dpkg is "aarch64" -- a side that holds
# nothing in the prefix, so "all" packages' dependencies would never resolve.
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
