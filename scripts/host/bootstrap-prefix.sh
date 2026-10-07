#!/bin/sh
# Restore a shipped core-deb to its full package set, from the mirror, inside
# the prefix itself (docs/spec/prefix-layers.md, "Minimal core-deb and its
# restore"). Run by the prefix's own bash after ship-prefix.sh:
#
#   bash $PREFIX/usr/lib/deb-native/scripts/runtime/bootstrap-prefix.sh
#
# 1. apt-get update from the sources the artifact carries;
# 2. apt-get install of every package named in $PREFIX/.dn/profile
#    (names only, no pinned versions);
# 3. check that libc6 and libc-bin are still held (the patched glibc files
#    must survive) and count the installed packages;
# 4. write $PREFIX/.dn/bootstrapped, so a second run is a no-op.
#
# The prefix is where this script lives, so nothing is passed in. Exit status
# is the result; re-running after a partial failure is safe (apt skips what is
# installed).
set -eu
# The prefix root: DN_INSTDIR when the caller set it; otherwise it is found
# from this script's own location.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DN=${DN_INSTDIR:-$(CDPATH= cd -- "$HERE/../../../../.." && pwd)}
PATH=$DN/usr/lib/deb-native/priv:$DN/usr/sbin:$DN/usr/bin:$DN/sbin:$DN/bin:$PATH
export PATH
# apt/dpkg put scratch files under $TMPDIR; the host's is outside the prefix
# (and not redirected), so dpkg could not stat what apt wrote. Keep them inside.
TMPDIR=$DN/tmp
export TMPDIR
mkdir -p "$TMPDIR"
PROFILE=$DN/.dn/profile
DONE=$DN/.dn/bootstrapped

[ -f "$PROFILE" ] || { echo "bootstrap: no $PROFILE" >&2; exit 1; }
if [ -f "$DONE" ]; then
  echo "bootstrap: already done ($(cat "$DONE"))"
  exit 0
fi

echo "bootstrap: apt-get update"
apt-get update

echo "bootstrap: installing $(grep -c . "$PROFILE") packages from the profile"
# shellcheck disable=SC2046
DEBIAN_FRONTEND=noninteractive apt-get install -y $(grep -v '^[[:space:]]*$' "$PROFILE" | grep -v '^#')

for h in libc6 libc-bin; do
  st=$(dpkg-query -W -f='${db:Status-Abbrev}' "$h" 2>/dev/null || true)
  case $st in
    hi*) ;;
    *) echo "bootstrap: $h is not held (status '$st'); the patched glibc is at risk" >&2; exit 1 ;;
  esac
done

n=$(dpkg-query -W -f='${db:Status-Abbrev}\n' | grep -c '^ii\|^hi')
echo "$n installed" > "$DONE"
echo "bootstrap: done, $n packages installed"
