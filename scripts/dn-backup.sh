#!/bin/sh
# Archive Termux's package state before true fusion (docs/true-fusion.md).
# dn-fuse.sh refuses to run without one of these. Covers apt's and dpkg's
# state and Termux's repo/mirror config -- not files packages later write
# into $PREFIX.
#
#   ~/dn-fusion-backup/termux-pkgstate-<ts>.tar.gz (+ .sha256)
#   ~/dn-fusion-backup/MANIFEST-<ts>.txt   sources, mirrors, dpkg arches,
#                                          full dpkg --get-selections
# Restore: cd $PREFIX && tar -xzf <archive>
#
# Usage: dn-backup.sh
set -eu
P=${PREFIX:-/data/data/com.termux/files/usr}
B="$HOME/dn-fusion-backup"
TS=$(date +%Y%m%d-%H%M%S)
F="$B/termux-pkgstate-$TS.tar.gz"
mkdir -p "$B"
cd "$P"
tar -czf "$F" --exclude='var/lib/dpkg/lock*' --exclude=var/lib/apt/lists/lock \
  --exclude=var/lib/apt/lists/partial \
  etc/apt etc/termux etc/alternatives var/lib/dpkg var/lib/apt
(cd "$B" && sha256sum "${F##*/}" > "${F##*/}.sha256" && sha256sum -c --quiet "${F##*/}.sha256")
{
  echo "# Termux package state before true fusion, $TS"
  echo "# restore: cd \$PREFIX && tar -xzf $F"
  echo; echo "## sources"; cat etc/apt/sources.list etc/apt/sources.list.d/* 2>/dev/null || true
  echo; echo "## chosen_mirrors"; cat etc/termux/chosen_mirrors 2>/dev/null || true
  echo; echo "## dpkg"
  echo "native=$(dpkg --print-architecture) foreign=$(dpkg --print-foreign-architectures | tr '\n' ' ')"
  echo "packages=$(dpkg -l | grep -c '^ii')"
  echo; echo "## selections"; dpkg --get-selections
} > "$B/MANIFEST-$TS.txt"
echo "dn-backup: $F"
