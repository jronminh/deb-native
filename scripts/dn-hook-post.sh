#!/bin/sh
# True fusion apt DPkg::Post-Invoke hook (docs/true-fusion.md): after dpkg
# ran, rewrite the absolute symlinks the just-installed packages brought in
# -- shipped ones, and the update-alternatives links their postinsts made --
# to relative ones inside the prefix. The kernel follows a symlink by itself,
# never re-entering the shim, so an absolute "/etc/alternatives/x" would
# resolve against Android's real root. Scoped to exactly those links:
# Termux's own prefix is never scanned.
#
# Never fails the apt run.
set -u
P=${PREFIX:-/data/data/com.termux/files/usr}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE="$P/var/lib/deb-native"
LOG="$P/var/log/deb-native-fusion.log"
[ -s "$STATE/pending" ] || [ -n "$(ls "$P"/lib/deb-native/bin 2>/dev/null)" ] || exit 0
touch "$STATE/pending"

LINKS=$(mktemp)
trap 'rm -f "$LINKS"' EXIT
sort -u "$STATE/pending" | while read -r pkg; do
  dpkg-query -L "$pkg:arm64" 2>/dev/null | while read -r f; do
    [ -L "$P$f" ] && echo "$P$f"
  done
done > "$LINKS"
# Alternatives links first, with Termux's gawk by full path: when the group
# is awk, `awk` itself is broken until they are fixed.
"$HERE/dn-fix-alternatives.sh" "$P"

# Programs the new packages put on a bin path get launchers (main's
# make-launchers.sh, scoped to exactly these files): run by name, a glibc
# program must not inherit Termux's Bionic LD_PRELOAD (termux-exec), and
# needs the path shim -- dn-run sets both.
PROGS=$(mktemp)
trap 'rm -f "$LINKS" "$PROGS"' EXIT
sort -u "$STATE/pending" | while read -r pkg; do
  dpkg-query -L "$pkg:arm64" 2>/dev/null | grep -E '^/(bin|sbin|games)/[^/]+$' | sed "s|^|$P|"
done > "$PROGS"
# Alternatives links on a bin path whose program is a Debian package's
# (bin/figlet -> etc/alternatives/figlet -> bin/figlet-figlet): found by
# name, they need a launcher too. Termux's own (bin/awk -> gawk) do not.
for a in "$P"/var/lib/dpkg/alternatives/*; do
  [ -f "$a" ] || continue
  n=0
  while IFS= read -r line; do
    n=$((n + 1))
    [ "$n" -eq 1 ] && continue
    [ -z "$line" ] && break
    [ "$n" -eq 2 ] || [ $((n % 2)) -eq 0 ] || continue
    l=${line#/usr}
    case "$l" in /bin/*|/sbin/*|/games/*) ;; *) continue ;; esac
    [ -L "$P$l" ] || continue
    real=$(readlink -f "$P$l") || continue
    owner=$(dpkg -S "${real#$P}" 2>/dev/null | head -1 | sed 's/: .*//')
    case "$owner" in *:arm64) echo "$P$l" ;; esac
  done < "$a"
done >> "$PROGS"

{
  echo "== $(date '+%F %T') post: $(sort -u "$STATE/pending" | tr '\n' ' ')"
  NORMALIZE_FUSE_USR=1 NORMALIZE_LINKS_FILE="$LINKS" "$HERE/normalize-symlinks.sh" "$P"
  [ -s "$PROGS" ] && DN_LAUNCH_FILES="$PROGS" "$HERE/make-launchers.sh" "$P"
  # Drop launchers whose program was removed.
  for l in "$P"/lib/deb-native/bin/*; do
    [ -f "$l" ] || continue
    real=$(sed -n 's/^exec "[^"]*" \(--trace \)\{0,1\}"\([^"]*\)".*/\2/p' "$l" | tail -1)
    [ -z "$real" ] || [ -e "$real" ] || { rm -f "$l"; echo "removed stale launcher $l"; }
  done
} >>"$LOG" 2>&1
: > "$STATE/pending"
exit 0
