#!/bin/sh
# apt's DPkg::Post-Invoke hook: after dpkg ran, make what was installed
# usable -- repoint new ELFs at Termux glibc, normalize absolute symlinks
# (required by the bind-only tracer, docs/bind-only.md), and regenerate the
# launcher wrappers (sudo-less's stage-4 "integrate", hooked into apt's
# lifecycle). Never fails an apt transaction.
#
# Usage: apt-hook-post.sh NEWPREFIX
set -eu
NEWPREFIX=${1:?usage: apt-hook-post.sh NEWPREFIX}
case "$NEWPREFIX" in /*) ;; *) NEWPREFIX="$PWD/$NEWPREFIX" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$NEWPREFIX/root"
LOG="$NEWPREFIX/var/log/deb-native-hook.log"
mkdir -p "$NEWPREFIX/var/log"

"$HERE/patch-elfs.sh" "$ROOT" >>"$LOG" 2>&1 || true
"$HERE/normalize-symlinks.sh" "$ROOT" >>"$LOG" 2>&1 || true
"$HERE/make-launchers.sh" "$ROOT" >>"$LOG" 2>&1 || true
