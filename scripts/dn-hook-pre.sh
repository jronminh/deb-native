#!/bin/sh
# The prefix's apt DPkg::Pre-Install-Pkgs hook (docs/design-0.2.0.md; from
# the naibed branch). apt feeds its plan on stdin in hook protocol version 3
# (set in the prefix's apt.conf):
#
#   VERSION 3
#   <apt config, one item per line>
#   <blank line>
#   pkg old-ver old-arch old-ma <|>|=|- new-ver new-arch new-ma action
#
# where action is the .deb path, **CONFIGURE** or **REMOVE**. For a manual
# `dpkg -i` (the routing dpkg wrapper), the .debs are given as arguments.
#
# Every .deb about to be unpacked is prepared in place, before dpkg sees it:
#   1. dn-translate-deb.sh: Architecture all -> arm64, custom/ fixes, ELFs
#      repointed at the libc6 stand-in;
#   2. collision check: refuse a file that exists in the prefix but no
#      package owns -- deb-native's own runtime and launchers;
#   3. patch-deb.sh: maintainer-script shebangs -> dn-shell.
# Any failure fails the hook, and apt then runs nothing.
#
# Usage: dn-hook-pre.sh PREFIX [DEB...]
set -eu
umask 022
DN=${1:?usage: dn-hook-pre.sh PREFIX [DEB...]}
shift
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
LOG="$DN/var/log/deb-native-hook.log"
mkdir -p "$DN/var/log"

# Output goes to the terminal, amid apt's own lines, and to the log.
logged() {
  rcf=$(mktemp)
  { "$@" 2>&1; echo $? > "$rcf"; } | tee -a "$LOG"
  rc=$(cat "$rcf"); rm -f "$rcf"
  return "$rc"
}

DEBS=$(mktemp)
trap 'rm -f "$DEBS"' EXIT
if [ $# -gt 0 ]; then
  for d; do echo "$d"; done > "$DEBS"
else
  # Keep the action lines (after the first blank line); their last field is
  # the .deb for anything being unpacked.
  awk 'body && $NF !~ /^\*\*/ { print $NF } /^$/ { body = 1 }' > "$DEBS"
fi

while IFS= read -r deb; do
  [ -n "$deb" ] || continue
  [ -f "$deb" ] || { echo "E: $deb not found" >&2; exit 1; }
  pkg=$(dpkg-deb -f "$deb" Package)
  echo "== $(date '+%F %T') $deb" >>"$LOG"
  logged "$HERE/dn-translate-deb.sh" "$deb" "$DN" \
    || { echo "E: translating $pkg failed" >&2; exit 1; }

  # Collision check against the prefix's own database.
  clash=$(dpkg-deb -c "$deb" | awk '{print $6}' | grep -v '/$' | sed 's|^\.||' |
    while IFS= read -r path; do
      [ -e "$DN$path" ] || [ -L "$DN$path" ] || continue
      if [ -d "$DN$path" ] && [ ! -L "$DN$path" ]; then continue; fi
      owner=$("$TP/bin/dpkg-query" --admindir="$DN/var/lib/dpkg" -S "$path" 2>&1) && continue
      printf ' %s' "$path"
    done) || true
  if [ -n "$clash" ]; then
    echo "E: $pkg would overwrite files no package owns (deb-native's own?):$clash" >&2
    exit 1
  fi

  logged "$HERE/patch-deb.sh" "$deb" "$DN" \
    || { echo "E: patching $pkg's maintainer scripts failed" >&2; exit 1; }
done < "$DEBS"
