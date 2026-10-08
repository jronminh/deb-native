#!/bin/sh
# Ship a prefix artifact: the one install path every host uses
# (docs/spec/prefix.md, "Ship"). Rows on the host's own shell with only POSIX
# sh and what toybox and coreutils both have (tar -z/-O, uname, df -P, mkdir,
# rm), so Android's mksh + toybox is enough.
#
#   1. read .dn/contract from the tarball without extracting it, and check it;
#   2. extract into DEST -- which must be the path the artifact was built for;
#   3. run the artifact's activation script (contract install=) with the host's
#      own shell: it checks the path and wires the session entry;
#   4. check the tree runs: start it through dn-trace for a trivial command.
# Any failure after DEST is created removes DEST. The host never edits a file
# inside the prefix; everything prefix-specific is the prefix's own scripts.
#
# Usage: ship-prefix.sh ARTIFACT.tar.gz DEST
set -eu
A=${1:?usage: ship-prefix.sh ARTIFACT.tar.gz DEST}
D=${2:?usage: ship-prefix.sh ARTIFACT.tar.gz DEST}
CONTRACT_VERSION=1

die() { echo "ship-prefix: $*" >&2; exit 1; }
[ -f "$A" ] || die "no such artifact: $A"
case "$D" in /*) ;; *) D="$PWD/$D" ;; esac
case "${D##*/}" in
  ''|*[!A-Za-z0-9._-]*) die "invalid prefix name: ${D##*/} (letters, digits, . _ -)" ;;
esac
[ ! -e "$D" ] || die "$D already exists"
parent=${D%/*}
[ -d "$parent" ] || die "no such directory: $parent"

# 1. The contract, read without extracting.
C=$(tar -xzOf "$A" ./.dn/contract 2>/dev/null) || die "$A carries no .dn/contract"
contract= name= arch= root= loader= install= entry= size=
# Line by line without a here-document: mksh writes those to a temporary
# file, and an app may have no writable TMPDIR.
oldifs=$IFS
IFS='
'
set -f
for l in $C; do
  case $l in
    '#'*) ;;
    contract=*) contract=${l#*=} ;;
    name=*) name=${l#*=} ;;
    arch=*) arch=${l#*=} ;;
    root=*) root=${l#*=} ;;
    loader=*) loader=${l#*=} ;;
    install=*) install=${l#*=} ;;
    entry=*) entry=${l#*=} ;;
    size=*) size=${l#*=} ;;
    desc=*|version=*) ;;
    *=*) echo "ship-prefix: warning: unknown contract key ${l%%=*}" >&2 ;;
  esac
done
IFS=$oldifs
set +f
[ "$contract" = "$CONTRACT_VERSION" ] || die "contract version '${contract:-?}' not supported (this host knows $CONTRACT_VERSION)"
[ -n "$name" ] && [ -n "$arch" ] && [ -n "$root" ] && [ -n "$loader" ] && [ -n "$entry" ] || die "contract lacks name/arch/root/loader/entry"
[ "$arch" = "$(uname -m)" ] || die "artifact is for $arch, this device is $(uname -m)"
# The artifact is built for one path and is not relocatable.
[ "$root" = "$D" ] || die "artifact is built for $root; install it there (it is not relocatable)"
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
if [ -n "$install" ]; then
  # Activation, the host's own shell (mksh + toybox on Android): it checks the
  # path and wires the session entry.  No tree program runs here.
  DN_INSTDIR=$D sh "$D/$install" || die "activation failed"
fi
# The acceptance test: the tree runs through dn-trace (a poor host starts
# dn-trace; nothing in the tree runs directly).  A trivial command, so it does
# not bootstrap or start a session.
RT=$D/usr/lib/deb-native
"$RT/dn-trace" "$D" "$D/$loader" -- /usr/bin/bash -c 'exit 0' \
  || die "the tree does not run under dn-trace"
trap - EXIT
echo "ship-prefix: $name installed in $D"
echo "ship-prefix: boot with:  cd $D && $entry"
