#!/bin/sh
# The prefix's apt DPkg::Post-Invoke hook (docs/spec/design.md): after dpkg
# ran, make what was installed usable.
#   1. alternatives links relative (dn-fix-alternatives.sh, Termux's gawk by
#      full path: when the group is awk, awk itself is broken until then);
#   2. every other absolute symlink in the prefix relative
#      (normalize-symlinks.sh): the kernel follows a link by itself, never
#      re-entering the shim, so /etc/x would resolve against Android's root;
#   3. launchers regenerated (make-launchers.sh).
# ELFs were already repointed in the package (dn-translate-deb.sh), so the
# 0.1.x post-install patch-elfs.sh step is gone. Never fails the apt run.
#
# Usage: dn-hook-post.sh PREFIX
set -u
DN=${1:?usage: dn-hook-post.sh PREFIX}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG="$DN/var/log/deb-native-hook.log"
mkdir -p "$DN/var/log"
# Output goes to the terminal, amid apt's own lines, and to the log (the
# timestamp only to the log).
echo "== $(date '+%F %T') post" >> "$LOG"
{
  "$HERE/dn-fix-alternatives.sh" "$DN"
  "$HERE/normalize-symlinks.sh" "$DN"
  "$HERE/../runtime/make-launchers.sh" "$DN"
} 2>&1 | tee -a "$LOG"
exit 0
