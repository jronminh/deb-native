#!/bin/sh
# Build a busybox bundle: dn-init + busybox-static + a program or two, with no
# Debian userland and no apt (docs/spec/variants.md, "A worked example").
#
# Usage: scripts/build-busybox.sh [OPTION...] BUILDER DEST
#   BUILDER            an existing Debian prefix that has apt and the shim (the
#                      builder; apt stays here, it does not enter the bundle)
#   DEST               where to make the bundle (must not exist yet)
#   --programs LIST    programs to ship, space-separated (default: fastfetch);
#                      each name is both the Debian package and the binary
#   --packages LIST    extra packages to install in the builder first
#   --runtime DIR      build-runtime.sh's output (default: build/, built if
#                      it holds no dn-init)
#   --no-shim          do not ship dn-lane.so (busybox is static; a dynamic
#                      program needs it)
#
# The bundle holds only what it needs: dn-init, busybox-static, the programs and
# their library closure, the loader, and a boot.sh/init.sh that run under busybox
# sh (no bash).  Run in Termux.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUNTIME=$ROOT/build PROGRAMS=fastfetch PACKAGES= SHIM=1
while [ $# -gt 0 ]; do
  case $1 in
    --programs) PROGRAMS=$2; shift 2 ;;
    --packages) PACKAGES="$PACKAGES $2"; shift 2 ;;
    --runtime) RUNTIME=$2; shift 2 ;;
    --no-shim) SHIM=0; shift ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "build-busybox: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 2 ] || { echo "usage: build-busybox.sh [OPTION...] BUILDER DEST  (--help)" >&2; exit 2; }
BUILDER=$(CDPATH= cd -- "$1" && pwd) || exit 2
DEST=$2
[ -x "$BUILDER/boot/boot.sh" ] || { echo "build-busybox: $BUILDER is not a prefix (no boot/boot.sh)" >&2; exit 2; }
[ -x "$BUILDER/usr/lib/deb-native/dn-init" ] || { echo "build-busybox: no dn-init in $BUILDER" >&2; exit 2; }
[ ! -e "$DEST" ] || { echo "build-busybox: $DEST already exists" >&2; exit 2; }

TMP=${TMPDIR:-${PREFIX:-/data/data/com.termux/files/usr}/tmp}/build-busybox.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
LIBDIR=usr/lib/aarch64-linux-gnu

[ -x "$RUNTIME/dn-init" ] || sh "$ROOT/scripts/build-runtime.sh" "$RUNTIME"

# ---- the packages, then the program's closure, in the builder ---------------
echo "build-busybox: installing busybox-static $PACKAGES $PROGRAMS in the builder"
if ! "$BUILDER/boot/boot.sh" sh -c \
     "apt-get install -y --no-install-recommends busybox-static $PACKAGES $PROGRAMS >/tmp/bb.log 2>&1"; then
  tail -15 "$BUILDER/tmp/bb.log" >&2 || true
  echo "build-busybox: apt-get failed in the builder" >&2
  exit 1
fi

echo "build-busybox: the closure"
"$BUILDER/boot/boot.sh" sh -c '
	for p in "$@"; do
		b=$(command -v "$p" 2>/dev/null) || { echo "MISSING $p"; continue; }
		echo "PROG $b"
		ldd "$b" 2>/dev/null | sed -n "s/.*=> \(\/[^ ]*\).*/LIB \1/p; s/^[[:space:]]*\(\/[^ ]*\) (0x.*/LIB \1/p"
	done
	echo "BUSYBOX $(command -v busybox 2>/dev/null || echo /bin/busybox)"
' sh $PROGRAMS > "$TMP/manifest" 2>/dev/null

# ---- assemble the bundle ----------------------------------------------------
mkdir -p "$DEST/boot" "$DEST/bin" "$DEST/etc" "$DEST/usr/bin" "$DEST/usr/lib/deb-native" "$DEST/$LIBDIR"
while read -r tag path; do
  case $tag in
    MISSING) echo "build-busybox: program '$path' not found in the builder" >&2; exit 1 ;;
    PROG)    mkdir -p "$DEST$(dirname "$path")"; cp "$BUILDER$path" "$DEST$path" ;;
    BUSYBOX) cp -L "$BUILDER$path" "$DEST/bin/busybox" ;;
    LIB)     cp -L "$BUILDER$path" "$DEST/$LIBDIR/$(basename "$path")" ;;
    '') ;;
    *)       echo "build-busybox: bad manifest line: $tag $path" >&2; exit 1 ;;
  esac
done < "$TMP/manifest"

cp "$RUNTIME/dn-init" "$DEST/usr/lib/deb-native/dn-init"
if [ "$SHIM" = 1 ]; then
  cp "$BUILDER/usr/lib/deb-native/dn-lane.so" "$DEST/usr/lib/deb-native/dn-lane.so"
fi
[ -f "$BUILDER/etc/os-release" ] && cp "$BUILDER/etc/os-release" "$DEST/etc/os-release"

# ---- the boot glue (busybox sh, guest paths, no bash) -----------------------
cat > "$DEST/boot/init.sh" <<'INIT'
# the bundle's init: busybox sh, guest paths.  No bash, no Debian.
PATH=/bin:/usr/bin; export PATH
HOME=/root; export HOME
LANG=C.UTF-8; export LANG
# a resolver: busybox's wget and friends read /etc/resolv.conf, and the tree
# ships none (Android supplies none either)
[ -e /etc/resolv.conf ] || printf 'nameserver 8.8.8.8\n' > /etc/resolv.conf
[ -d /tmp ] || mkdir -p /tmp
[ -d /root ] || mkdir -p /root
if [ $# -gt 0 ]; then exec "$@"; fi
exec /bin/busybox sh
INIT

cat > "$DEST/boot/boot.sh" <<'BOOT'
#!/system/bin/sh
# The bundle's entry: dn-init + busybox, this directory's parent is the root.
root=$(CDPATH= cd -- "${0%/*}/.." && pwd) || exit 1
unset LD_PRELOAD LD_LIBRARY_PATH
cd "$root" || exit 1
umask 022
tz=$(getprop persist.sys.timezone 2>/dev/null)
exec env -i \
  PATH=/bin:/usr/bin \
  HOME=/root \
  SHELL=/bin/busybox \
  LANG=C.UTF-8 \
  TERM="${TERM:-dumb}" \
  TZ="${tz:-UTC}" \
  "$root/usr/lib/deb-native/dn-init" ${DN_TINY_FLAGS:-} -t "$root" -- /bin/busybox sh /boot/init.sh "$@"
BOOT

chmod +x "$DEST/boot/boot.sh" "$DEST/boot/init.sh" \
         "$DEST/bin/busybox" "$DEST/usr/lib/deb-native/dn-init" 2>/dev/null || true
[ "$SHIM" = 1 ] && chmod +x "$DEST/usr/lib/deb-native/dn-lane.so" 2>/dev/null || true
for p in $PROGRAMS; do [ -f "$DEST/usr/bin/$p" ] && chmod +x "$DEST/usr/bin/$p"; done

# ---- the applets as their own names -----------------------------------------
# One symlink per applet (sh, grep, tar, ...), so a tool that calls another by
# name (tar calls gzip) finds it; the multi-call binary stays the only copy.
"$BUILDER/boot/boot.sh" /bin/busybox --list 2>/dev/null | while read -r a; do
  [ "$a" = busybox ] && continue
  [ -e "$DEST/bin/$a" ] && continue
  ln -sf busybox "$DEST/bin/$a"
done

# ---- what the bundle is (so a boot menu can name it) ------------------------
VER=$(cat "$ROOT/VERSION" 2>/dev/null || echo unknown)
{
  echo "# deb-native bundle (docs/spec/variants.md)"
  echo "format=2"
  echo "name=busybox"
  echo "desc=busybox bundle ($PROGRAMS) for deb-native $VER"
  echo "version=$VER"
  echo "arch=aarch64"
  echo "shim=$SHIM"
} > "$DEST/boot/version"

printf 'build-busybox: %s (%s)\n' "$DEST" "$(du -sh "$DEST" | cut -f1)"
