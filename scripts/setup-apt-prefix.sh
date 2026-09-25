#!/bin/sh
# Point Termux's own apt at a real Debian arm64 repository, scoped to a
# separate prefix — the piece design-install-path.md always intended but
# this repo never actually built until the survey
# (docs/findings-survey-2026-09-25.md) showed it was the #1 gap: without
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
# as-is — see "Open work" in docs/design-install-path.md.
set -eu
NEWPREFIX=${1:?usage: setup-apt-prefix.sh NEWPREFIX [suite]}
SUITE=${2:-stable}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Build the path-redirect shim from source if it is missing (it is a build
# artifact; see scripts/build-path-redirect.sh for the toolchain notes).
[ -f "$HERE/../native/path-redirect.so" ] || "$HERE/build-path-redirect.sh"

mkdir -p "$NEWPREFIX/etc/apt/apt.conf.d" "$NEWPREFIX/etc/apt/sources.list.d" \
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
deb [trusted=yes] https://deb.debian.org/debian $SUITE main
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
EOF

"$HERE/native-seed.sh" "$NEWPREFIX/var/lib/dpkg"
APT_CONFIG="$NEWPREFIX/etc/apt.conf" apt-get update

echo "==> bootstrapping base packages (dash, debconf, cdebconf, ...) in one"
echo "    transaction -- see scripts/bootstrap-base.sh for why one, not"
echo "    piecemeal, matters here"
"$HERE/bootstrap-base.sh" "$NEWPREFIX"

echo "==> ready: APT_CONFIG=$NEWPREFIX/etc/apt.conf apt-get install -y <package>"
