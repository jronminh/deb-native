#!/bin/sh
# Build a prefix: a Debian arm64 tree that boots under dn-tiny (docs/building.md).
# Run in Termux, as the Termux user; no root, no proot at run time.
#
#   1. debootstrap --foreign: fetch and unpack the base packages (Termux's
#      debootstrap; its stage-two script is fixed first, see below)
#   2. put the runtime in the tree: usr/lib/deb-native/dn-tiny, boot/boot.sh,
#      boot/init.sh
#   3. debootstrap --second-stage under dn-tiny: the packages' scripts run as
#      they would on Debian
#   4. apt sources, then the shim (dn-lane.so) compiled in the tree against its
#      own glibc, then the chosen packages
#
# Usage: scripts/build-prefix.sh [OPTION...] DEST
#   --set NAME         a suggested package set (default: minimal)
#                        minimal  the debootstrap base and apt
#                        dev      + build-essential git curl ca-certificates
#                                   python3 nano less procps file
#                        full     dev + openssh-client wget vim-tiny unzip
#                                   xz-utils man-db htop
#   --packages LIST    more packages, space-separated (any Debian package)
#   --suite SUITE      Debian suite (default: trixie)
#   --mirror URL       Debian mirror (default: http://deb.debian.org/debian)
#   --variant NAME     debootstrap variant (default: minbase)
#   --runtime DIR      the output of build-runtime.sh (default: build/, built
#                      when it holds no dn-tiny)
#   --name NAME        the prefix's name in boot/version (default: DEST's name)
#   --no-shim          no dn-lane.so: every path call stops in the tracer
#                      (slower; for comparison or a toolchain-free build)
#   --ldhook           ship dn-ldhook.so, the loader hook: the loader's own
#                      paths are mapped (needs execmod/execmem)
#   --cancel           patch glibc's shared cancellable svc at its build-time
#                      offset (find-cancellable.py): 204 fewer stops in the Lua
#                      build, about 1% slower (the hook taxes every cancellable
#                      call); off by default
#   --keep-toolchain   keep gcc and libc6-dev after building the shim (the dev
#                      and full sets keep them anyway)
#
#   DN_TINY_FLAGS      dn-tiny's options for the build (default: -s 500, poll
#                      for the next stop: the second stage stops a lot)
#
# Needs: debootstrap (pkg install debootstrap), the runtime's own build
# needs (clang or gcc, python3), network.  DEST must not exist yet.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

SET=minimal PACKAGES= SUITE=trixie MIRROR=http://deb.debian.org/debian VARIANT=minbase
RUNTIME=$ROOT/build NAME= SHIM=1 LDHOOK=0 CANCEL_PATCH=0 KEEP_TC=0
while [ $# -gt 0 ]; do
  case $1 in
    --set) SET=$2; shift 2 ;;
    --packages) PACKAGES="$PACKAGES $2"; shift 2 ;;
    --suite) SUITE=$2; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    --variant) VARIANT=$2; shift 2 ;;
    --runtime) RUNTIME=$2; shift 2 ;;
    --name) NAME=$2; shift 2 ;;
    --no-shim) SHIM=0; shift ;;
    --ldhook) LDHOOK=1; shift ;;
    --cancel) CANCEL_PATCH=1; shift ;;
    --keep-toolchain) KEEP_TC=1; shift ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "build-prefix: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 1 ] || { echo "usage: build-prefix.sh [OPTION...] DEST  (--help)" >&2; exit 2; }
DEST=$1

case $SET in
  minimal) SET_PKGS= ;;
  dev) SET_PKGS="build-essential git curl ca-certificates python3 nano less procps file" ;;
  full) SET_PKGS="build-essential git curl ca-certificates python3 nano less procps file
                  openssh-client wget vim-tiny unzip xz-utils man-db htop" ;;
  *) echo "build-prefix: --set is minimal, dev or full" >&2; exit 2 ;;
esac
case $SET in minimal) ;; *) KEEP_TC=1 ;; esac

DN_TINY_FLAGS=${DN_TINY_FLAGS--s 500}
export DN_TINY_FLAGS

say() { printf '\n== %s\n' "$*"; }
die() { echo "build-prefix: $*" >&2; exit 1; }

command -v debootstrap >/dev/null 2>&1 || die "debootstrap not found (pkg install debootstrap)"
[ -e "$DEST" ] && die "$DEST exists; give a new directory"
mkdir -p "$DEST"
DEST=$(CDPATH= cd -- "$DEST" && pwd)
[ -n "$NAME" ] || NAME=${DEST##*/}

# ---- the runtime
if [ ! -x "$RUNTIME/dn-tiny" ]; then
  say "building the runtime"
  sh "$ROOT/scripts/build-runtime.sh" "$RUNTIME"
fi

# ---- 1. the first stage
say "debootstrap, first stage: $SUITE ($VARIANT) from $MIRROR"
debootstrap --foreign --arch=arm64 --variant="$VARIANT" "$SUITE" "$DEST" "$MIRROR"

# Termux's debootstrap replaces chown, useradd, usermod, groupadd, chage, chfn,
# dpkg-statoverride and others with empty files in the second stage (they fail
# under proot).  They work under dn-tiny: keep them.
for f in "$DEST"/debootstrap/*; do
  [ -f "$f" ] && sed -i '/Replace problematic binaries with a stub/d; /^[[:space:]]*echo "" > "\$TARGET\//d' "$f"
done

# ---- 2. the runtime in the tree
mkdir -p "$DEST/usr/lib/deb-native" "$DEST/boot"
cp "$RUNTIME/dn-tiny" "$DEST/usr/lib/deb-native/dn-tiny"
if [ "$LDHOOK" = 1 ]; then
  [ -f "$RUNTIME/dn-ldhook.so" ] ||
    die "no dn-ldhook.so in $RUNTIME (run scripts/build-runtime.sh)"
  cp "$RUNTIME/dn-ldhook.so" "$DEST/usr/lib/deb-native/dn-ldhook.so"
  chmod 755 "$DEST/usr/lib/deb-native/dn-ldhook.so"
fi
cp "$ROOT/scripts/prefix/boot.sh" "$ROOT/scripts/prefix/init.sh" "$DEST/boot/"
chmod 755 "$DEST/usr/lib/deb-native/dn-tiny" "$DEST/boot/boot.sh" "$DEST/boot/init.sh"

# ---- 3. the second stage, under dn-tiny
say "debootstrap, second stage (under dn-tiny)"
# shellcheck disable=SC2086
if ! env -i HOME=/root TERM=dumb PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 \
     "$DEST/usr/lib/deb-native/dn-tiny" -q $DN_TINY_FLAGS -t "$DEST" -- /bin/sh /debootstrap/debootstrap --second-stage; then
  tail -n 30 "$DEST/debootstrap/debootstrap.log" >&2 2>/dev/null || true
  die "the second stage failed (log: $DEST/debootstrap/debootstrap.log)"
fi
# The first stage unpacked through proot --link2symlink: its .l2s files are
# left over once dpkg has unpacked the packages again.  Remove them unless
# something still points at one.
find "$DEST" -xdev -name '.l2s.*' 2>/dev/null | while read -r f; do
  if [ -z "$(find "$DEST" -xdev -lname "*${f##*/}" ! -name '.l2s.*' 2>/dev/null | head -n 1)" ]; then
    rm -f "$f"
  fi
done

# ---- 4. apt, the shim, the packages
case $MIRROR in */) MIRROR=${MIRROR%/} ;; esac
SECURITY=${MIRROR%/debian}/debian-security
cat > "$DEST/etc/apt/sources.list.d/debian.sources" <<SRC
Types: deb
URIs: $MIRROR
Suites: $SUITE $SUITE-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: $SECURITY
Suites: $SUITE-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
SRC
rm -f "$DEST/etc/apt/sources.list"

run() { "$DEST/boot/boot.sh" "$@"; }

say "apt update"
run apt-get update

if [ "$SHIM" = 1 ]; then
  say "the shim: dn-lane.so, built in the tree against its glibc"
  run apt-get install -y --no-install-recommends gcc libc6-dev
  mkdir -p "$DEST/tmp/dn-lane"
  # the shim is lane.c, and the law (policy.c) it shares with the tracer, built
  # against the tree's glibc (-DDN_LIBC: the types, the strings and pol_readlink)
  cp "$ROOT/src/dn-tiny/lane.c" "$ROOT/src/dn-tiny/lane.h" \
     "$ROOT/src/dn-tiny/policy.c" "$ROOT/src/dn-tiny/policy.h" "$DEST/tmp/dn-lane/"
  if [ ! -f "$RUNTIME/gen/policy-rows.inc" ]; then
    mkdir -p "$RUNTIME/gen"
    python3 "$ROOT/src/dn-tiny/gen_rows.py" "$RUNTIME/gen/policy-rows.inc"
  fi
  cp "$RUNTIME/gen/policy-rows.inc" "$DEST/tmp/dn-lane/"
  # the shared cancellable svc (glibc's __syscall_cancel_arch): its offset is
  # found here, from the tree's own libc, because at run time `mov x8, x6` is
  # not unique.  One patch covers open, read, write and the rest; without it
  # that path stops in the tracer
  CANCEL=
  if [ "$CANCEL_PATCH" = 1 ]; then
    if off=$(python3 "$ROOT/scripts/find-cancellable.py" \
             "$DEST/usr/lib/aarch64-linux-gnu/libc.so.6" 2>/dev/null); then
      off=${off##*offset }
      off=${off%% *}
      CANCEL="-DDN_CANCEL_OFF=$off"
      say "the cancellable svc: ${off} (one patch for open, read, write, ...)"
    else
      say "the cancellable svc: not found; that path stays with the tracer"
    fi
  fi
  run gcc -O2 -Wall -Wextra -shared -fPIC -DDN_LIBC $CANCEL -o /usr/lib/deb-native/dn-lane.so \
      /tmp/dn-lane/lane.c /tmp/dn-lane/policy.c
  rm -rf "$DEST/tmp/dn-lane"
  if [ "$KEEP_TC" = 0 ]; then
    run apt-get purge -y --autoremove gcc libc6-dev
  fi
fi

if [ -n "$(echo $SET_PKGS $PACKAGES)" ]; then
  say "packages: $(echo $SET_PKGS $PACKAGES)"
  # shellcheck disable=SC2086
  run apt-get install -y $SET_PKGS $PACKAGES
fi
run apt-get clean

# ---- what the prefix is
VER=$(cat "$ROOT/VERSION")
cat > "$DEST/boot/version" <<V
# deb-native prefix (docs/building.md)
format=2
name=$NAME
desc=Debian $SUITE ($VARIANT, set $SET) for deb-native $VER
version=$VER
arch=aarch64
suite=$SUITE
shim=$SHIM
ldhook=$LDHOOK
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
size=$(du -sm "$DEST" | cut -f1)
V

say "done: $DEST ($(du -sh "$DEST" | cut -f1))"
echo "boot it:    $DEST/boot/boot.sh"
echo "run a command:  $DEST/boot/boot.sh dpkg -l"
