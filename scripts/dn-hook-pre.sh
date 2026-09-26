#!/bin/sh
# True fusion apt DPkg::Pre-Install-Pkgs hook (docs/true-fusion.md, "Install
# pipeline"). apt feeds its full plan on stdin in hook protocol version 3
# (set by dn-fuse.sh):
#
#   VERSION 3
#   <apt config, one item per line>
#   <blank line>
#   pkg old-ver old-arch old-ma <|>|=|- new-ver new-arch new-ma action
#
# where action is the .deb path, **CONFIGURE** or **REMOVE**.
#
# 1. Plan guard (tier 0): refuse the whole run if it would remove or
#    crossgrade a floor package (dn-floor.sh, snapshotted by dn-fuse.sh) --
#    what the Termux app, apt and dpkg run on. Other Termux packages may be
#    replaced by Debian's (a crossgrade: foo:aarch64 -> foo:arm64).
# 2. Translate every .deb about to be unpacked, in place, before dpkg sees
#    it: fuse-repack.sh (usr/ merged into the flat prefix, Architecture all
#    -> arm64, ELFs repointed at the libc6:arm64 identity paths),
#    fuse-classify.sh (refuse any file collision), patch-deb.sh (maintainer
#    script shebangs -> dn-shell).
# 3. Record the packages for dn-hook-post.sh.
#
# Any failure fails the hook, and apt then runs nothing: in Termux's own
# prefix a half-translated install is worse than none.
set -eu
umask 022
P=${PREFIX:-/data/data/com.termux/files/usr}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE="$P/var/lib/deb-native"
LOG="$P/var/log/deb-native-debian-mode.log"
mkdir -p "$STATE" "$(dirname "$LOG")"
PLAN=$(mktemp)
trap 'rm -f "$PLAN"' EXIT

# Keep only the action lines (after the first blank line).
awk 'body { print } /^$/ { body = 1 }' > "$PLAN"
{ echo "== $(date '+%F %T') plan"; cat "$PLAN"; } >>"$LOG"

FLOOR="$STATE/floor"
[ -s "$FLOOR" ] || { echo "dn-hook-pre: no floor snapshot ($FLOOR); run dn-fuse.sh" >&2; exit 1; }
refused=""
replacing=""
while read -r pkg ov oa oma cmp nv na nma action; do
  [ -n "$pkg" ] || continue
  # A Termux package (old arch not arm64; "-"/"none" = new install) removed
  # or replaced by another architecture.
  if [ "$oa" != - ] && [ "$oa" != none ] && [ "$oa" != arm64 ]; then
    case "$action" in
      '**CONFIGURE**') ;;
      '**REMOVE**') grep -qxF "$pkg" "$FLOOR" && refused="$refused $pkg:$oa(remove)" ;;
      *) if [ "$na" != "$oa" ]; then
           if grep -qxF "$pkg" "$FLOOR"; then refused="$refused $pkg:$oa(->$na)"
           else replacing="$replacing $pkg"; fi
         fi ;;
    esac
  fi
  # Nothing but arm64 may be installed.
  case "$action" in
    '**REMOVE**'|'**CONFIGURE**') ;;
    *) [ "$na" = arm64 ] || [ "$na" = all ] || refused="$refused $pkg:$na(install)" ;;
  esac
done < "$PLAN"
if [ -n "$refused" ]; then
  echo "dn-hook-pre: refusing: this plan would change Termux's floor (tier 0):$refused" >&2
  exit 1
fi
# fuse-classify.sh may let a crossgraded package overwrite its own old files.
export DN_REPLACING="$replacing"

while read -r pkg ov oa oma cmp nv na nma action; do
  case "$action" in '**REMOVE**'|'**CONFIGURE**'|'') continue ;; esac
  [ -f "$action" ] || { echo "dn-hook-pre: $action not found" >&2; exit 1; }
  echo "== translate $action" >>"$LOG"
  "$HERE/fuse-repack.sh" "$action" >>"$LOG" 2>&1 \
    || { echo "dn-hook-pre: fuse-repack failed for $pkg (see $LOG)" >&2; exit 1; }
  "$HERE/fuse-classify.sh" "$action" >>"$LOG" 2>&1 \
    || { echo "dn-hook-pre: $pkg would overwrite existing files, refusing:" >&2; grep REFUSE "$LOG" | tail -5 >&2; exit 1; }
  "$HERE/patch-deb.sh" "$action" "$P" >>"$LOG" 2>&1 \
    || { echo "dn-hook-pre: patch-deb failed for $pkg (see $LOG)" >&2; exit 1; }
  echo "$pkg" >> "$STATE/pending"
done < "$PLAN"
