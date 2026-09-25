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
# dpkg itself in the explicit phases the pipeline needs:
#   1. --unpack every downloaded .deb
#   2. grun --configure every new ELF (dash included -- it must already
#      run via Termux's glibc before step 3 execs it)
#   3. patch-maintainer-scripts.sh: rewrite hardcoded paths, and point
#      each script's shebang at this prefix's own (now-runnable) dash
#      instead of the real, root-owned /system/bin/sh
#   4. --configure -a, with LD_PRELOAD=path-redirect.so and DN_INSTDIR
#      exported -- dpkg preserves the environment into maintainer
#      scripts (confirmed by direct test, docs/design-manual-overlay.md),
#      so dash inherits both and redirects its own open64/stat64/execve
#      calls for any /usr,/etc,/var,/opt path to this prefix.
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

echo "==> patching new ELF binaries with grun --configure"
find "$NEWPREFIX/root" -type f -perm -u+x 2>/dev/null | while IFS= read -r f; do
  case "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) grun --configure "$f" >/dev/null 2>&1 || true ;;
  esac
done

echo "==> patching maintainer scripts' hardcoded absolute paths and shebang"
"$HERE/patch-maintainer-scripts.sh" "$NEWPREFIX/var/lib/dpkg" "$NEWPREFIX/root"

echo "==> configuring"
# NOT exporting LD_PRELOAD/DN_INSTDIR here: dpkg itself is a Bionic
# process, and Bionic's linker refuses to even start it with a glibc .so
# in LD_PRELOAD ("CANNOT LINK EXECUTABLE ... library libc.so.6 not
# found" -- confirmed by direct test). patch-maintainer-scripts.sh
# points each script's shebang at a small wrapper instead, which sets
# both only for the dash process it execs -- dpkg's own environment
# stays untouched.
dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-not-root --force-script-chrootless --force-architecture \
     --configure -a || true
