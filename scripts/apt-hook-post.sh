#!/bin/sh
# apt's DPkg::Post-Invoke hook: after dpkg ran, make what was installed
# usable -- regenerate the launcher wrappers and the `dn` front-end for every
# program now in the prefix (sudo-less's stage-4 "integrate", hooked into
# apt's lifecycle). Never fails an apt transaction.
#
# Usage: apt-hook-post.sh NEWPREFIX
set -eu
NEWPREFIX=${1:?usage: apt-hook-post.sh NEWPREFIX}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$NEWPREFIX/root"
LOG="$NEWPREFIX/var/log/deb-native-hook.log"
mkdir -p "$NEWPREFIX/var/log"

"$HERE/patch-elfs.sh" "$ROOT" >>"$LOG" 2>&1 || true
"$HERE/make-launchers.sh" "$ROOT" >>"$LOG" 2>&1 || true
