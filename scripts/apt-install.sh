#!/bin/sh
# Install a package (and its real dependencies) into a prefix set up by
# setup-apt-prefix.sh.
#
# Split into explicit phases rather than a single `apt-get install`,
# because apt was found (the hard way — see docs/findings-hard-package,
# ruby-adsf) to run its own dpkg in ONE combined invocation that unpacks
# and configures every package internally, back to back, with no apt-level
# hook point in between: a DPkg::Pre-Invoke hook fires once, before
# anything is unpacked, and never again for that transaction. There is no
# apt-side seam to patch maintainer scripts between unpack and configure.
#
# So apt is used only to resolve the dependency graph and download the
# .debs (--download-only, no dpkg call at all); this script then drives
# dpkg itself in the two explicit phases the pipeline needs:
#   1. --unpack every downloaded .deb
#   2. patch-maintainer-scripts.sh across the whole admindir
#   3. --configure -a
#
# Usage: apt-install.sh $NEWPREFIX package [package...]
set -eu
NEWPREFIX=${1:?usage: apt-install.sh NEWPREFIX package...}
shift
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCHIVES="$NEWPREFIX/var/cache/apt/archives"

echo "==> resolving and downloading (no install yet)"
APT_CONFIG="$NEWPREFIX/etc/apt.conf" apt-get install -y --no-install-recommends \
  --download-only "$@"

debs=$(find "$ARCHIVES" -maxdepth 1 -name '*.deb')
[ -n "$debs" ] || { echo "nothing to install (already installed?)"; exit 0; }

echo "==> unpacking $(printf '%s\n' "$debs" | wc -l) package(s)"
# shellcheck disable=SC2086
dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-not-root --force-script-chrootless --force-architecture \
     --unpack $debs

echo "==> patching maintainer scripts' hardcoded absolute paths"
"$HERE/patch-maintainer-scripts.sh" "$NEWPREFIX/var/lib/dpkg" "$NEWPREFIX/root"

echo "==> configuring"
dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-not-root --force-script-chrootless --force-architecture \
     --configure -a || true

echo "==> patching new ELF binaries with grun --configure"
find "$NEWPREFIX/root" -type f -perm -u+x 2>/dev/null | while IFS= read -r f; do
  case "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) grun --configure "$f" >/dev/null 2>&1 || true ;;
  esac
done
