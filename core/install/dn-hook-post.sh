#!/bin/sh
# The prefix's apt DPkg::Post-Invoke hook (docs/spec/design.md): after dpkg
# ran, make what was installed usable.
#   1. alternatives links relative (dn-fix-alternatives.sh, Termux's gawk by
#      full path: when the group is awk, awk itself is broken until then);
#   2. every other absolute symlink in the prefix relative
#      (normalize-symlinks.sh): the kernel follows a link by itself, never
#      re-entering the shim, so /etc/x would resolve against Android's root;
#   3. launchers regenerated (make-launchers.sh);
#   4. gcc's own default dynamic-linker pointed at the prefix's glibc loader
#      (dn-fix-gcc-specs.sh), so a plain `gcc -o prog prog.c` produces a
#      binary that actually runs, not just one that links.
# ELFs were already repointed in the package (dn-translate-deb.sh), so the
# 0.1.x post-install patch-elfs.sh step is gone. Never fails the apt run.
#
# Usage: dn-hook-post.sh PREFIX
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The prefix is where this hook lives: $PREFIX/usr/lib/deb-native/scripts/install.
# An explicit PREFIX argument still wins (the in-place bootstrap passes one).
if [ -n "${1:-}" ] && [ -d "$1/usr/lib/deb-native" ]; then DN=$1; else DN=$(CDPATH= cd -- "$HERE/../../../../.." && pwd); fi
LOG="$DN/var/log/deb-native-hook.log"
mkdir -p "$DN/var/log"
# Output goes to the terminal, amid apt's own lines, and to the log (the
# timestamp only to the log).
echo "== $(date '+%F %T') post" >> "$LOG"
{
  "$HERE/dn-fix-glibc.sh" "$DN"
  "$HERE/dn-fix-alternatives.sh" "$DN"
  "$HERE/normalize-symlinks.sh" "$DN"
  "$HERE/../runtime/make-launchers.sh" "$DN"
  "$HERE/dn-fix-gcc-specs.sh" "$DN"
} 2>&1 | tee -a "$LOG"
exit 0
