#!/bin/sh
# Complete a shipped core-deb: install the packages named in /.dn/profile from
# the mirror, inside the prefix.  Run by the prefix's init on its first boot,
# under dn-trace -- so every path here is a **guest** path.
#   bash /usr/lib/deb-native/scripts/runtime/bootstrap-prefix.sh
# Re-running after a partial failure is safe (apt skips what is installed).
set -eu
PATH=/usr/lib/deb-native/priv:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# apt/dpkg put scratch files under $TMPDIR; keep them inside the tree.
TMPDIR=/tmp
export TMPDIR
mkdir -p "$TMPDIR"
PROFILE=/.dn/profile
DONE=/.dn/bootstrapped

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
