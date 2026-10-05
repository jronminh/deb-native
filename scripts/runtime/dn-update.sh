#!/system/bin/sh
# dn-update -- install a prebuilt deb-native component into the prefix.
#
# deb-native's own overlay (fused glibc, the shim, the Bionic runtime, the
# hooks/priv/launchers) is not a Debian package, so `apt` cannot update it.
# This is the constrained primitive that can: copy one already-built file (or
# directory) over its fixed location, within an allowlist. Base packages are
# never writable through it.
#
# The runtime is path-agnostic (the loader/shim self-derive the prefix; dn-run
# is Bionic), so a straight copy is enough -- no rebake.
#
# Usage:
#   dn-update list
#   dn-update <component> <file> [sha256]
#   dn-update <dir-component> <dir>
set -eu

INST=${DN_INSTDIR:-}
if [ -z "$INST" ]; then
  echo "dn-update: DN_INSTDIR not set -- run inside the deb-native userland" >&2
  exit 1
fi

usage() { echo "usage: dn-update list | dn-update <component> <file|dir> [sha256]" >&2; }

target() {
  case "$1" in
    loader)         echo "$INST/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" ;;
    libc)           echo "$INST/usr/lib/aarch64-linux-gnu/libc.so.6" ;;
    libresolv)      echo "$INST/usr/lib/aarch64-linux-gnu/libresolv.so.2" ;;
    libnsl)         echo "$INST/usr/lib/aarch64-linux-gnu/libnsl.so.1" ;;
    libnss-compat)  echo "$INST/usr/lib/aarch64-linux-gnu/libnss_compat.so.2" ;;
    libnss-hesiod)  echo "$INST/usr/lib/aarch64-linux-gnu/libnss_hesiod.so.2" ;;
    librt)          echo "$INST/usr/lib/aarch64-linux-gnu/librt.so.1" ;;
    ldconfig)       echo "$INST/usr/sbin/ldconfig" ;;
    localedef)      echo "$INST/usr/bin/localedef" ;;
    iconv)          echo "$INST/usr/bin/iconv" ;;
    shim)           echo "$INST/usr/lib/deb-native/path-redirect.so" ;;
    run)            echo "$INST/usr/lib/deb-native/dn-run" ;;
    trace)          echo "$INST/usr/lib/deb-native/dn-trace" ;;
    shell)          echo "$INST/usr/bin/dn-shell" ;;
    perl)           echo "$INST/usr/bin/dn-perl" ;;
    libtalloc)      echo "$INST/usr/lib/deb-native/host/libtalloc.so.2" ;;
    libtermux-exec) echo "$INST/usr/lib/deb-native/host/libtermux-exec-ld-preload.so" ;;
    hooks)          echo "$INST/usr/lib/deb-native/scripts" ;;
    priv)           echo "$INST/usr/lib/deb-native/priv" ;;
    launchers)      echo "$INST/usr/lib/deb-native/bin" ;;
    custom)         echo "$INST/usr/lib/deb-native/custom" ;;
    *) return 1 ;;
  esac
}

case "${1:-}" in
  ""|-h|--help) usage; exit 2 ;;
  list)
    echo "files: loader libc libresolv libnsl libnss-compat libnss-hesiod librt"
    echo "       ldconfig localedef iconv shim run trace shell perl"
    echo "       libtalloc libtermux-exec"
    echo "dirs:  hooks priv launchers custom"
    exit 0 ;;
esac

comp=$1
src=${2:-}
[ -n "$src" ] || { usage; exit 2; }
dst=$(target "$comp") || { echo "dn-update: unknown component '$comp'" >&2; exit 2; }

if [ -n "${3:-}" ]; then
  got=$(sha256sum "$src" | awk '{print $1}')
  [ "$got" = "$3" ] || { echo "dn-update: sha256 mismatch (got $got)" >&2; exit 1; }
fi

case "$comp" in
  hooks|priv|launchers|custom)
    [ -d "$src" ] || { echo "dn-update: $comp needs a directory" >&2; exit 1; }
    mkdir -p "$dst"
    cp -a "$src/." "$dst/"
    echo "dn-update: $comp <- $src -> $dst" ;;
  *)
    [ -f "$src" ] || { echo "dn-update: no such file: $src" >&2; exit 1; }
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst.tmp"
    chmod 755 "$dst.tmp"
    mv -f "$dst.tmp" "$dst"
    echo "dn-update: $comp <- $src -> $dst" ;;
esac
