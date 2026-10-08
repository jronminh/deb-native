#!/bin/sh
# install-from-release -- fetch a prefix tarball from GitHub and ship it.
#
# The host's one-command install (docs/spec/prefix.md, "Ship"): download a
# rolling `prefix` release asset, read its contract's root=, and hand the
# tarball to the one install path, ship-prefix.sh.  The artifact is built for
# one path and is not relocatable, so the destination is the contract's root,
# never a guess.
#
# Runs on the host's own shell (POSIX sh + toybox): toybox wget or curl,
# tar -xzO.  ship-prefix.sh is used from this directory when present (a
# checkout) or fetched from the repo otherwise, so the script also works when
# copied alone onto a poor host.
#
# Usage: install-from-release.sh [core-deb|core-ultra]   (default core-deb)
#
# Host environment:
#   DN_REPO           owner/repo        (default jronminh/deb-native)
#   DN_RELEASE        release tag       (default prefix)
#   DN_REF            git ref for the fetched ship-prefix.sh (default main)
#   DN_TMPDIR         download dir      (default ${TMPDIR:-/data/local/tmp})
#   DN_SESSION_SHELL  passed to activation (see scripts/host/install-prefix.sh)
set -eu

usage() {
  echo "usage: install-from-release.sh [core-deb|core-ultra]   (default core-deb)" >&2
  echo "env: DN_REPO DN_RELEASE DN_REF DN_TMPDIR DN_SESSION_SHELL" >&2
}
die() { echo "install-from-release: $*" >&2; exit 1; }

NAME=${1:-core-deb}
case $NAME in
  core-deb|core-ultra) ;;
  -h|--help) usage; exit 0 ;;
  *) die "unknown artifact '$NAME' (core-deb, core-ultra)" ;;
esac

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

# The contract's root= names the one path the artifact may live at; read it
# without extracting the tree (as ship-prefix.sh itself does).
ROOT=$(tar -xzOf "$ART" ./.dn/contract 2>/dev/null | sed -n 's/^root=//p')
[ -n "$ROOT" ] || die "$NAME.tar.gz carries no root= in .dn/contract"

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

echo "install-from-release: shipping $NAME to $ROOT"
sh "$SHIP" "$ART" "$ROOT"
