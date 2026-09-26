#!/bin/sh
# Point Termux's own apt at a real Debian arm64 repository, scoped to a
# separate prefix — the piece design.md always intended but
# this repo never actually built until the survey
# (docs/findings.md) showed it was the #1 gap: without
# it, only the one requested .deb ever gets installed, never its
# dependencies.
#
# Usage: setup-apt-prefix.sh $NEWPREFIX [debian-suite (default: stable)]
#
# After this, use apt-get (or scripts/apt-install.sh) with:
#   APT_CONFIG=$NEWPREFIX/etc/apt.conf apt-get install -y <package>
#
# KNOWN INSECURE SHORTCUT: sources.list uses [trusted=yes], skipping
# signature verification entirely, because Termux ships no Debian archive
# keyring to verify against. Fine for this prototype; do not ship this
# as-is — see "Open work" in docs/design.md.
set -eu
NEWPREFIX=${1:?usage: setup-apt-prefix.sh NEWPREFIX [suite]}
SUITE=${2:-stable}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/.." && pwd)

# Safety: never write apt config into Termux's own prefix. Pointing this at
# $PREFIX overwrites Termux's sources.list and makes its repo disappear.
TERMUX_PREFIX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
case "$NEWPREFIX" in
  "$TERMUX_PREFIX"|"$TERMUX_PREFIX"/*)
    echo "setup-apt-prefix: refusing NEWPREFIX=$NEWPREFIX" >&2
    echo "  it is inside Termux's prefix ($TERMUX_PREFIX); that would clobber Termux's apt." >&2
    echo "  Use a separate prefix, e.g. \$HOME/.dn (the installer's default)." >&2
    exit 1 ;;
esac

# Build the path-redirect shim from source if it is missing (it is a build
# artifact; see scripts/build-path-redirect.sh for the toolchain notes).
[ -f "$HERE/../native/path-redirect.so" ] || "$HERE/build-path-redirect.sh"

mkdir -p "$NEWPREFIX/etc/apt/apt.conf.d" "$NEWPREFIX/etc/apt/sources.list.d" \
         "$NEWPREFIX/etc/apt/preferences.d" \
         "$NEWPREFIX/etc/apt/trusted.gpg.d" \
         "$NEWPREFIX/var/lib/apt/lists/partial" \
         "$NEWPREFIX/var/cache/apt/archives/partial" \
         "$NEWPREFIX/var/lib/dpkg" \
         "$NEWPREFIX/root/etc" "$NEWPREFIX/root/usr" \
         "$NEWPREFIX/root/var/lib" "$NEWPREFIX/root/var/log" \
         "$NEWPREFIX/root/var/cache" "$NEWPREFIX/root/opt"
[ -f "$NEWPREFIX/var/lib/dpkg/status" ] || : > "$NEWPREFIX/var/lib/dpkg/status"
touch "$NEWPREFIX/etc/apt/trusted.gpg"

cat > "$NEWPREFIX/etc/apt/sources.list" <<EOF
deb [trusted=yes] https://deb.debian.org/debian $SUITE main contrib non-free-firmware
deb [trusted=yes] https://deb.debian.org/debian ${SUITE}-updates main contrib non-free-firmware
deb [trusted=yes] https://security.debian.org/debian-security ${SUITE}-security main contrib non-free-firmware
EOF

cat > "$NEWPREFIX/etc/apt.conf" <<EOF
Dir::State "$NEWPREFIX/var/lib/apt";
Dir::State::status "$NEWPREFIX/var/lib/dpkg/status";
Dir::Cache "$NEWPREFIX/var/cache/apt";
Dir::Etc "$NEWPREFIX/etc/apt";
Dir::Etc::sourcelist "$NEWPREFIX/etc/apt/sources.list";
Dir::Etc::sourceparts "$NEWPREFIX/etc/apt/sources.list.d";
Dir::Etc::trusted "$NEWPREFIX/etc/apt/trusted.gpg";
Dir::Etc::trustedparts "$NEWPREFIX/etc/apt/trusted.gpg.d";
APT::Architecture "arm64";
APT::Architectures:: "arm64";
Dpkg::options:: "--instdir=$NEWPREFIX/root";
Dpkg::options:: "--admindir=$NEWPREFIX/var/lib/dpkg";
Dpkg::options:: "--force-not-root";
Dpkg::options:: "--force-script-chrootless";
Dpkg::options:: "--force-architecture";
// Hook the deb-native pipeline into apt's own lifecycle (the sudo-less
// approach): patch each .deb before dpkg unpacks it, and regenerate
// launchers after. So a plain apt-get install (through the arch-aware apt
// wrapper, which points APT_CONFIG here for Debian-only names) installs
// Debian arm64 packages seamlessly.
DPkg::Pre-Install-Pkgs { "$REPO/scripts/apt-hook-pre.sh $NEWPREFIX"; };
DPkg::Post-Invoke { "$REPO/scripts/apt-hook-post.sh $NEWPREFIX || true"; };
EOF

"$HERE/native-seed.sh" "$NEWPREFIX/var/lib/dpkg"
APT_CONFIG="$NEWPREFIX/etc/apt.conf" apt-get update

echo "==> bootstrapping base packages (dash, debconf, cdebconf, ...) in one"
echo "    transaction -- see scripts/bootstrap-base.sh for why one, not"
echo "    piecemeal, matters here"
"$HERE/bootstrap-base.sh" "$NEWPREFIX"

echo "==> generating launchers, the dn front-end, and activating PATH"
"$HERE/make-launchers.sh" "$NEWPREFIX/root"
"$HERE/make-apt-wrappers.sh" "$NEWPREFIX/root"
"$HERE/dn-activate.sh" "$NEWPREFIX/root"

echo "==> ready: APT_CONFIG=$NEWPREFIX/etc/apt.conf apt-get install -y <package>"
