#!/bin/sh
# Complete a shipped core-deb: install the packages named in /.dn/profile from
# the mirror, inside the prefix.  Run by the prefix's init on its first boot,
# under dn-trace -- so every path here is a **guest** path.
#   bash /usr/lib/deb-native/scripts/runtime/bootstrap-prefix.sh
# Re-running after a partial failure is safe (apt skips what is installed).
set -eu
PATH=/usr/lib/deb-native/priv:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# No terminal to ask on: the package scripts take debconf's defaults.
DEBIAN_FRONTEND=noninteractive
export DEBIAN_FRONTEND
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

# 1. Configure the shipped packages.  The build unpacks them but cannot run
#    their scripts, so they are "unpacked" (debootstrap's second stage does
#    the same): run what dpkg's unpack would have run first -- the preinst
#    "install" of each never-configured package, base-passwd's first (it
#    writes /etc/passwd and /etc/group) -- then configure them all.
# base-files' postinst wants /mnt a directory: a dangling mnt -> ../mnt (no
# host-provided sibling) becomes one.
if [ -L /mnt ] && [ ! -e /mnt ]; then
  rm -f /mnt
  mkdir /mnt
fi
fresh=$(dpkg-query -W -f='${db:Status-Abbrev} ${binary:Package} ${Config-Version}\n' \
  | awk '$1 ~ /^.U/ && NF == 2 { print $2 }')
if [ -n "$fresh" ]; then
  echo "bootstrap: configuring $(echo "$fresh" | wc -l) shipped packages"
  for p in $(echo "$fresh" | grep '^base-passwd$'; echo "$fresh" | grep -v '^base-passwd$'); do
    s=/var/lib/dpkg/info/$p.preinst
    [ -x "$s" ] || continue
    DPKG_MAINTSCRIPT_PACKAGE=${p%%:*} DPKG_MAINTSCRIPT_NAME=preinst \
      DPKG_MAINTSCRIPT_ARCH=$(dpkg-query -W -f='${Architecture}' "$p") DPKG_ROOT= \
      "$s" install
  done
  dpkg --configure -a
fi

echo "bootstrap: apt-get update"
apt-get update

echo "bootstrap: installing $(grep -c . "$PROFILE") packages from the profile"
# shellcheck disable=SC2046
DEBIAN_FRONTEND=noninteractive apt-get install -y $(grep -v '^[[:space:]]*$' "$PROFILE" | grep -v '^#')

# The CA bundle (/etc/ssl/certs/ca-certificates.crt) is generated, never
# shipped: build it once, whether or not a trigger already did.
if command -v update-ca-certificates >/dev/null 2>&1; then
  update-ca-certificates
fi

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
