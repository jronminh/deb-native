#!/bin/sh
# Ship a prefix artifact: the one install path every host uses
# (docs/spec/prefix.md, "Ship"). Rows on the host's own shell with only POSIX
# sh and what toybox and coreutils both have (tar -z/-O, uname, df -P, mkdir,
# rm), so Android's mksh + toybox is enough.
#
#   1. read boot/version from the tarball without extracting it, and check it;
#   2. extract into DEST;
#   3. run the artifact's activation script, boot/install.sh, with the host's
#      own shell: it checks the path and wires the session entry;
#   4. check the tree runs: start it through boot/boot.sh for a trivial command.
# Any failure after DEST is created removes DEST. The host never edits a file
# inside the prefix; everything prefix-specific is the prefix's own scripts.
#
# The boot/ paths are the convention (docs/spec/prefix.md, "The boot/
# directory"): boot/boot.sh is the entry, boot/install.sh the activation,
# boot/version the manifest.  There is no contract: the same facts live in
# boot/version, which -- like the contract did -- is readable from the tarball
# before extracting it.
#
# Usage: ship-prefix.sh ARTIFACT.tar.gz DEST
set -eu
A=${1:?usage: ship-prefix.sh ARTIFACT.tar.gz DEST}
D=${2:?usage: ship-prefix.sh ARTIFACT.tar.gz DEST}
FORMAT_VERSION=2
ENTRY=boot/boot.sh
INSTALL=boot/install.sh

die() { echo "ship-prefix: $*" >&2; exit 1; }
[ -f "$A" ] || die "no such artifact: $A"
case "$D" in /*) ;; *) D="$PWD/$D" ;; esac
case "${D##*/}" in
  ''|*[!A-Za-z0-9._-]*) die "invalid prefix name: ${D##*/} (letters, digits, . _ -)" ;;
esac
[ ! -e "$D" ] || die "$D already exists"
parent=${D%/*}
[ -d "$parent" ] || die "no such directory: $parent"

# 1. The manifest, read without extracting.
C=$(tar -xzOf "$A" ./boot/version 2>/dev/null) || die "$A carries no boot/version"
format= name= arch= root= size=
# Line by line without a here-document: mksh writes those to a temporary
# file, and an app may have no writable TMPDIR.
oldifs=$IFS
IFS='
'
set -f
for l in $C; do
  case $l in
    '#'*) ;;
    format=*) format=${l#*=} ;;
    name=*) name=${l#*=} ;;
    arch=*) arch=${l#*=} ;;
    root=*) root=${l#*=} ;;
    size=*) size=${l#*=} ;;
    desc=*|version=*) ;;
    *=*) echo "ship-prefix: warning: unknown boot/version key ${l%%=*}" >&2 ;;
  esac
done
IFS=$oldifs
set +f
[ "$format" = "$FORMAT_VERSION" ] || die "artifact layout '${format:-?}' not supported (this host knows $FORMAT_VERSION)"
[ -n "$name" ] && [ -n "$arch" ] || die "boot/version lacks name/arch"
[ "$arch" = "$(uname -m)" ] || die "artifact is for $arch, this device is $(uname -m)"
# A current artifact names no root and installs anywhere (dn-monitor derives the
# root from its own location).  One that still carries root= is the old fixed
# form: it only works at that path.
[ -z "$root" ] || [ "$root" = "$D" ] \
  || die "artifact is built for $root; install it there (it is not relocatable)"
if [ -n "$size" ]; then
  # POSIX format (-P): one line per filesystem, available KiB in field 4.
  # Compare in MiB: Android's mksh does 32-bit arithmetic, so size * 1024
  # could overflow.
  avail=$(df -kP "$parent" | { read -r _; read -r _ _ _ a _; echo "${a:-0}"; })
  [ $((avail / 1024)) -ge "$size" ] || die "needs ${size} MiB in $parent, $((avail / 1024)) MiB free"
fi

# 2-4. Extract, activate, check; undo on failure.
mkdir "$D"
trap 'rm -rf "$D"' EXIT
tar -xzf "$A" -C "$D" || die "extract failed"
# Activation, the host's own shell (mksh + toybox on Android): it checks the
# path and wires the session entry.  No tree program runs here.
DN_INSTDIR=$D sh "$D/$INSTALL" || die "activation failed"
# The acceptance test: the tree runs through its entry, boot/boot.sh -- run with
# the prefix root as the working directory, as any host runs it.  A trivial
# command, so it does not bootstrap or start a session.
( cd "$D" && sh "$ENTRY" /usr/bin/bash -c 'exit 0' ) \
  || die "the tree does not run through $ENTRY"
trap - EXIT
echo "ship-prefix: $name installed in $D"
echo "ship-prefix: boot with:  cd $D && $ENTRY"
