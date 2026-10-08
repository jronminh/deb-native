#!/bin/sh
# install-from-release -- fetch a prefix tarball from GitHub and ship it.
#
# The host's one-command install (docs/spec/prefix.md, "Ship"): download a
# rolling `prefix` release asset and hand it to the one install path,
# ship-prefix.sh.  The artifact is relocatable (dn-trace derives the root from
# its own location), so DEST is a parameter; a fixed-root artifact that still
# carries root= uses that path instead.
#
# Runs on the host's own shell (POSIX sh + toybox): toybox wget or curl,
# tar -xzO.  ship-prefix.sh is used from this directory when present (a
# checkout) or fetched from the repo otherwise, so the script also works when
# copied alone onto a poor host.
#
# Usage: install-from-release.sh [core-deb|core-ultra] [DEST]   (default core-deb)
#        DEST default: the contract's root= (a fixed-root artifact), else ./<name>
#
# Host environment:
#   DN_REPO           owner/repo        (default jronminh/deb-native)
#   DN_RELEASE        release tag       (default prefix)
#   DN_REF            git ref for the fetched ship-prefix.sh (default main)
#   DN_TMPDIR         download dir      (default ${TMPDIR:-/data/local/tmp})
#   DN_DEST           install path when DEST is not given
#   DN_SESSION_SHELL  passed to activation (see scripts/host/install-prefix.sh)
set -eu

usage() {
  echo "usage: install-from-release.sh [core-deb|core-ultra] [DEST]   (default core-deb)" >&2
  echo "env: DN_REPO DN_RELEASE DN_REF DN_TMPDIR DN_DEST DN_SESSION_SHELL" >&2
}
die() { echo "install-from-release: $*" >&2; exit 1; }

NAME=${1:-core-deb}
case $NAME in
  core-deb|core-ultra) ;;
  -h|--help) usage; exit 0 ;;
  *) die "unknown artifact '$NAME' (core-deb, core-ultra)" ;;
esac
DEST=${2:-${DN_DEST:-}}

REPO=${DN_REPO:-jronminh/deb-native}
TAG=${DN_RELEASE:-prefix}
REF=${DN_REF:-main}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# downloader: URL OUT.
fetch() {
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  else
    die "neither curl nor wget found"
  fi
}

# Where to keep the download.  A poor host may have no writable /tmp.
TMP=${DN_TMPDIR:-${TMPDIR:-/data/local/tmp}}
if [ ! -d "$TMP" ] || [ ! -w "$TMP" ]; then
  TMP=${HOME:-.}
fi
[ -d "$TMP" ] && [ -w "$TMP" ] || die "no writable temp dir (set DN_TMPDIR)"
ART=$TMP/$NAME.tar.gz
trap 'rm -f "$ART"' EXIT

URL="https://github.com/$REPO/releases/download/$TAG/$NAME.tar.gz"
echo "install-from-release: fetching $URL"
fetch "$URL" "$ART" || die "download failed: $URL"

# The install path: DEST (or DN_DEST); else a fixed-root artifact's root=
# (read without extracting, as ship-prefix.sh does); else ./<name>.
if [ -z "$DEST" ]; then
  ROOT=$(tar -xzOf "$ART" ./.dn/contract 2>/dev/null | sed -n 's/^root=//p')
  DEST=${ROOT:-$PWD/$NAME}
fi

# The one install path: from this checkout, or fetched for a lone copy.
if [ -f "$HERE/ship-prefix.sh" ]; then
  SHIP=$HERE/ship-prefix.sh
else
  SHIP=$TMP/ship-prefix.sh
  echo "install-from-release: fetching ship-prefix.sh ($REF)"
  fetch "https://raw.githubusercontent.com/$REPO/$REF/scripts/host/ship-prefix.sh" "$SHIP" \
    || die "download failed: ship-prefix.sh"
  chmod 755 "$SHIP"
fi

echo "install-from-release: shipping $NAME to $DEST"
sh "$SHIP" "$ART" "$DEST"
