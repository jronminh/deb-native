#!/bin/sh
# Make every update-alternatives link in the prefix relative
# (docs/spec/design.md). update-alternatives writes
# absolute targets (usr/bin/awk -> /etc/alternatives/awk -> /usr/bin/mawk);
# the kernel follows those against Android's real root, so the command is
# gone until they are rewritten -- and when the group is awk, so is every
# script that would rewrite them (reinstalling mawk broke awk exactly this
# way). So this runs right after each update-alternatives call
# (the priv/ wrapper setup-runtime.sh writes), with Termux's own gawk by
# full path: never an alternatives link.
#
# Usage: dn-fix-alternatives.sh PREFIX
set -u
P=${1:?usage: dn-fix-alternatives.sh PREFIX}
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
# Called from a maintainer script, PATH has glibc tools first while this
# Bionic shell may carry termux-exec's preload: use Termux's own tools.
PATH="$TP/bin:$PATH"
AWK="$TP/bin/gawk"
ADMIN="$P/var/lib/dpkg/alternatives"
[ -d "$ADMIN" ] || exit 0

fix() {  # $1: a link path as update-alternatives records it (/usr/bin/awk)
  l=$P$1
  [ -L "$l" ] || return 0
  t=$(readlink "$l")
  case "$t" in /usr/*|/etc/*|/var/*|/opt/*|/bin/*|/sbin/*|/lib/*) ;; *) return 0 ;; esac
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
