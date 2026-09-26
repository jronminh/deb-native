#!/bin/sh
# True fusion (docs/true-fusion.md): make every update-alternatives link in
# the fused prefix relative. update-alternatives writes portable absolute
# targets (bin/awk -> /etc/alternatives/awk -> /bin/gawk); the kernel
# follows those against Android's real root, so the command is gone until
# they are rewritten -- and when the group is awk, so is every script that
# would rewrite them (normalize-symlinks.sh's relpath is awk). So this runs
# right after each update-alternatives call (the fusion-bin wrapper) and
# uses Termux's own gawk by full path: a floor package, never an
# alternatives link.
#
# Usage: dn-fix-alternatives.sh [PREFIX]
set -u
P=${1:-${PREFIX:-/data/data/com.termux/files/usr}}
# Run from a maintainer script, PATH puts Termux's glibc coreutils first
# while this Bionic shell carries termux-exec's preload, which a glibc
# program cannot load: use Termux's own tools.
PATH="$P/bin:$PATH"
AWK="$P/bin/gawk"
ADMIN="$P/var/lib/dpkg/alternatives"
[ -d "$ADMIN" ] || exit 0

fix() {  # $1: link path as recorded (/usr/bin/awk, /etc/alternatives/awk)
  l=${1#/usr}; l=$P$l
  [ -L "$l" ] || return 0
  t=$(readlink "$l")
  case "$t" in /usr/*|/usr) t=${t#/usr} ;; /etc/*|/var/*|/opt/*|/bin/*|/sbin/*|/lib/*|/share/*) ;; *) return 0 ;; esac
  rel=$("$AWK" -v from="$(dirname "$l")" -v to="$P$t" 'BEGIN {
    nf = split(from, a, "/"); nt = split(to, b, "/"); i = 1
    while (i <= nf && i <= nt && a[i] == b[i]) i++
    out = ""
    for (j = i; j <= nf; j++) out = out (out == "" ? "" : "/") ".."
    for (j = i; j <= nt; j++) out = out (out == "" ? "" : "/") b[j]
    print (out == "" ? "." : out) }')
  ln -sfn "$rel" "$l"
}

for a in "$ADMIN"/*; do
  [ -f "$a" ] || continue
  # Line 1 mode, line 2 master link, then slave name/link pairs up to a
  # blank line; each name also has /etc/alternatives/<name>.
  n=0
  while IFS= read -r line; do
    n=$((n + 1))
    [ "$n" -eq 1 ] && continue
    [ -z "$line" ] && break
    if [ "$n" -eq 2 ] || [ $((n % 2)) -eq 0 ]; then fix "$line"
    else fix "/etc/alternatives/$line"
    fi
  done < "$a"
  fix "/etc/alternatives/${a##*/}"
done
exit 0
