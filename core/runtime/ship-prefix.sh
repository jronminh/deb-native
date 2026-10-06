#!/bin/sh
# Ship a prefix artifact: the one install path every host uses
# (docs/spec/prefix-contract.md, "What the host does"). Runs on the host's
# own shell with only POSIX sh and what toybox and coreutils both have
# (tar -z/-O, uname, df -P, mkdir, rm), so Android's mksh + toybox is enough.
#
#   1. read .dn/contract from the tarball without extracting it, and check it;
#   2. extract into DEST;
#   3. run the prefix's own relocation script (contract relocate=), if any;
#   4. check that the prefix's shell runs (contract entry=, with -c 'exit 0');
#   5. run the artifact's activation script (contract install=), host shell;
#   6. run the artifact's completion script (contract bootstrap=) through the
#      prefix's own shell -- the prefix installs .dn/profile from the mirror.
# Any failure after DEST is created removes DEST. The host never edits a
# file inside the prefix; everything prefix-specific is the prefix's own
# relocation script.
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
contract= name= arch= root= loader= relocate= entry= size=
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
    relocate=*) relocate=${l#*=} ;;
    install=*) install=${l#*=} ;;
    bootstrap=*) bootstrap=${l#*=} ;;
    entry=*) entry=${l#*=} ;;
    size=*) size=${l#*=} ;;
    desc=*|version=*) ;;
    *=*) echo "ship-prefix: warning: unknown contract key ${l%%=*}" >&2 ;;
  esac
done
IFS=$oldifs
set +f
[ "$contract" = "$CONTRACT_VERSION" ] || die "contract version '${contract:-?}' not supported (this host knows $CONTRACT_VERSION)"
[ -n "$name" ] && [ -n "$arch" ] && [ -n "$root" ] && [ -n "$entry" ] || die "contract lacks name/arch/root/entry"
[ "$arch" = "$(uname -m)" ] || die "artifact is for $arch, this device is $(uname -m)"
if [ -n "$relocate" ]; then
  [ -n "$loader" ] || die "contract has relocate= but no loader="
else
  # Not relocatable: it runs only at root. Compare with symlinks resolved
  # on the parent (/data/user/0/... and /data/data/... are one directory).
  rparent=$(cd -P -- "$parent" && pwd)
  bparent=$(cd -P -- "${root%/*}" 2>/dev/null && pwd) || bparent=${root%/*}
  [ "$rparent/${D##*/}" = "$bparent/${root##*/}" ] || die "not relocatable: installs only at $root"
fi
if [ -n "$size" ]; then
  # POSIX format (-P): one line per filesystem, available KiB in field 4.
  # Compare in MiB: Android's mksh does 32-bit arithmetic, so size * 1024
  # could overflow.
  avail=$(df -kP "$parent" | { read -r _; read -r _ _ _ a _; echo "${a:-0}"; })
  [ $((avail / 1024)) -ge "$size" ] || die "needs ${size} MiB in $parent, $((avail / 1024)) MiB free"
fi

# 2-6. Extract, relocate, check, activate, complete; undo on failure.
mkdir "$D"
trap 'rm -rf "$D"' EXIT
tar -xzf "$A" -C "$D" || die "extract failed"
if [ -n "$relocate" ]; then
  DN_INSTDIR=$D sh "$D/$relocate" || die "relocation failed"
fi
"$D/${entry%% *}" -c 'exit 0' || die "the prefix's shell ($D/${entry%% *}) does not run"
if [ -n "$install" ]; then
  # Host-side activation, the host's own shell (mksh + toybox on Android).
  DN_INSTDIR=$D sh "$D/$install" "$D" || die "activation failed"
fi
if [ -n "$bootstrap" ]; then
  # Completion, the prefix's own shell: it has apt and coreutils, the host
  # does not need them.
  DN_INSTDIR=$D "$D/usr/bin/bash" "$D/$bootstrap" || die "completion failed"
fi
trap - EXIT
echo "ship-prefix: $name installed in $D"
