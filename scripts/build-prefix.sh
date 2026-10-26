#!/bin/sh
# Build a prefix: a Debian arm64 tree that boots under dn-init (docs/building.md).
# Run in Termux, as the Termux user; no root, no proot at run time.
#
#   1. debootstrap --foreign: fetch and unpack the base packages (Termux's
#      debootstrap; its stage-two script is fixed first, see below)
#   2. put the runtime in the tree: usr/lib/deb-native/{dn-init,dn-exec,dn-lane.so},
#      boot/boot.sh, boot/init.sh
#   3. debootstrap --second-stage under dn-init: the packages' scripts run as
#      they would on Debian
#   4. apt sources, then the chosen packages
#
# Nothing is compiled here: the three runtime artifacts come from a runtime
# build (scripts/build-runtime.sh, scripts/build-shim.sh), so this needs no
# compiler in the tree -- a minimal prefix has none.  A runtime that lacks one
# of them is refused rather than installed.
#
# Usage: scripts/build-prefix.sh [OPTION...] DEST
#   --set NAME         a suggested package set (default: minimal)
#                        minimal  the debootstrap base and apt
#                        dev      + build-essential git curl ca-certificates
#                                   python3 nano less procps file
#                        full     dev + openssh-client wget vim-tiny unzip
#                                   xz-utils man-db htop
#   --packages LIST    more packages, space-separated (any Debian package)
#   --suite SUITE      Debian or Ubuntu suite (default: trixie)
#   --mirror URL       Debian or Ubuntu mirror (default: http://deb.debian.org/debian)
#   --distro NAME      debian or ubuntu (default: from the mirror's name)
#   --variant NAME     debootstrap variant (default: minbase)
#   --host NAME        app (the default) or adb: an adb prefix carries the
#                      runtime built with --host adb and boots in the shell
#                      domain (no Android seccomp); the prefix is built with
#                      the app runtime, then the adb one is put in place
#   --runtime DIR      the app runtime used to build (default: build/)
#   --runtime-adb DIR  the adb runtime for --host adb (default: build-adb/,
#                      built when it holds no dn-init)
#   --name NAME        the prefix's name in boot/version (default: DEST's name)
#
#   DN_TINY_FLAGS      dn-init's options for the build (default: -s 500, poll
#                      for the next stop: the second stage stops a lot)
#
# The shim's own switches (--cancel, --libc) live in scripts/build-shim.sh now;
# --no-shim and --keep-toolchain are gone with the in-tree build.
#
# Needs: debootstrap (pkg install debootstrap), a built runtime, network.
# DEST must not exist yet.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
say() { printf '\n== %s\n' "$*"; }
die() { echo "build-prefix: $*" >&2; exit 1; }

SET=minimal PACKAGES= SUITE=trixie MIRROR=http://deb.debian.org/debian VARIANT=minbase DISTRO=auto
RUNTIME=$ROOT/build RUNTIME_ADB=$ROOT/build-adb HOST=app NAME=
while [ $# -gt 0 ]; do
  case $1 in
    --set) SET=$2; shift 2 ;;
    --packages) PACKAGES="$PACKAGES $2"; shift 2 ;;
    --suite) SUITE=$2; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    --distro) DISTRO=$2; shift 2 ;;
    --variant) VARIANT=$2; shift 2 ;;
    --runtime) RUNTIME=$2; shift 2 ;;
    --runtime-adb) RUNTIME_ADB=$2; shift 2 ;;
    --host) HOST=$2; shift 2 ;;
    --name) NAME=$2; shift 2 ;;
    --no-shim) die "--no-shim is gone: a prefix always carries the shim. A tree without one is a variant's business (scripts/build-busybox.sh)" ;;
    --cancel) die "--cancel moved to scripts/build-shim.sh: the shim is built with the runtime now, and its offset belongs to that build" ;;
    --keep-toolchain) die "--keep-toolchain is gone: the shim is no longer built in the tree, so no toolchain is installed or purged. Use --packages gcc if you want one" ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "build-prefix: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 1 ] || { echo "usage: build-prefix.sh [OPTION...] DEST  (--help)" >&2; exit 2; }
DEST=$1
case $HOST in
  app|adb) ;;
  *) echo "build-prefix: --host is app or adb" >&2; exit 2 ;;
esac

case $SET in
  minimal) SET_PKGS= ;;
  dev) SET_PKGS="build-essential git curl ca-certificates python3 nano less procps file" ;;
  full) SET_PKGS="build-essential git curl ca-certificates python3 nano less procps file
                  openssh-client wget vim-tiny unzip xz-utils man-db htop" ;;
  *) echo "build-prefix: --set is minimal, dev or full" >&2; exit 2 ;;
esac

DN_TINY_FLAGS=${DN_TINY_FLAGS--s 500}
export DN_TINY_FLAGS
# the build's own commands (debootstrap's second stage, apt) run as root:
# the built prefix still boots as the app's real uid by default
DN_INITIAL=root
export DN_INITIAL

command -v debootstrap >/dev/null 2>&1 || die "debootstrap not found (pkg install debootstrap)"
if [ "$DISTRO" = auto ]; then
  case $MIRROR in *ubuntu*) DISTRO=ubuntu ;; *) DISTRO=debian ;; esac
fi
case $DISTRO in
  debian) DISTRO_NAME=Debian ;;
  ubuntu) DISTRO_NAME=Ubuntu ;;
  *) die "--distro is debian or ubuntu" ;;
esac
[ -e "$DEST" ] && die "$DEST exists; give a new directory"
mkdir -p "$DEST"
DEST=$(CDPATH= cd -- "$DEST" && pwd)
[ -n "$NAME" ] || NAME=${DEST##*/}

# ---- the runtimes
# The build always runs under the app runtime: the second stage and the shim
# are built in Termux, where Android's app seccomp filter is active.  An adb
# prefix only swaps in the shell/ADB runtime once everything is built.
if [ ! -x "$RUNTIME/dn-init" ]; then
  say "building the app runtime"
  sh "$ROOT/scripts/build-runtime.sh" --host app "$RUNTIME"
fi
[ "$(cat "$RUNTIME/host" 2>/dev/null || echo app)" = app ] ||
  die "the runtime in $RUNTIME is not the app runtime (see --runtime)"
if [ "$HOST" = adb ]; then
  if [ ! -x "$RUNTIME_ADB/dn-init" ]; then
    say "building the adb runtime"
    sh "$ROOT/scripts/build-runtime.sh" --host adb "$RUNTIME_ADB"
  fi
  [ "$(cat "$RUNTIME_ADB/host" 2>/dev/null)" = adb ] ||
    die "the runtime in $RUNTIME_ADB is not the adb runtime (see --runtime-adb)"
fi

# ---- 1. the first stage
say "debootstrap, first stage: $DISTRO_NAME $SUITE ($VARIANT) from $MIRROR"
# Ubuntu's Release is signed by a key Termux does not ship; the tree gets
# ubuntu-keyring in the second stage, which apt uses.  A host keyring, when
# present, is used instead of skipping the check.
KEYARG=
if [ "$DISTRO" = ubuntu ]; then
  if [ -f /data/data/com.termux/files/usr/share/keyrings/ubuntu-archive-keyring.gpg ]; then
    KEYARG=--keyring=/data/data/com.termux/files/usr/share/keyrings/ubuntu-archive-keyring.gpg
  else
    KEYARG=--no-check-sig
    say "Ubuntu: no keyring on the host; the Release signature is not checked here"
  fi
fi
# shellcheck disable=SC2086
debootstrap $KEYARG --foreign --arch=arm64 --variant="$VARIANT" "$SUITE" "$DEST" "$MIRROR"

# Termux's debootstrap replaces chown, useradd, usermod, groupadd, chage, chfn,
# dpkg-statoverride and others with empty files in the second stage (they fail
# under proot).  They work under dn-init: keep them.  It also names its own
# prefix absolutely (/data/data/com.termux/files/usr), a path the tree does not
# have (the narrowed host rows): point every one at the tree's own /usr, and its
# fallback directory at /debootstrap (where these copies are).
for f in "$DEST"/debootstrap/*; do
  [ -f "$f" ] || continue
  sed -i -e 's|/data/data/com.termux/files/usr|/usr|g' \
         -e 's|DEBOOTSTRAP_DIR=/usr/share/debootstrap|DEBOOTSTRAP_DIR=/debootstrap|' \
         -e '/Replace problematic binaries with a stub/d' \
         -e '/^[[:space:]]*echo "" > "\$TARGET\//d' "$f"
done

# ---- 2. the runtime in the tree
# All three artifacts, and a runtime that is missing one is refused here rather
# than installed: without dn-exec the gate answers policy_dnexec() with an empty
# path and silently falls back to the loader, and without the shim every path
# call stops in the tracer.  A tree must run what it says it runs.
[ -x "$RUNTIME/dn-exec" ] || die "$RUNTIME has no dn-exec; rebuild the runtime (scripts/build-runtime.sh)"
[ -f "$RUNTIME/dn-lane.so" ] || die "$RUNTIME has no dn-lane.so; build it with scripts/build-shim.sh"
mkdir -p "$DEST/usr/lib/deb-native" "$DEST/boot"
cp "$RUNTIME/dn-init" "$RUNTIME/dn-exec" "$RUNTIME/dn-lane.so" "$DEST/usr/lib/deb-native/"
cp "$ROOT/scripts/prefix/boot.sh" "$ROOT/scripts/prefix/init.sh" "$DEST/boot/"
chmod 755 "$DEST/usr/lib/deb-native/dn-init" "$DEST/usr/lib/deb-native/dn-exec" \
          "$DEST/usr/lib/deb-native/dn-lane.so" \
          "$DEST/boot/boot.sh" "$DEST/boot/init.sh"
# The user tools that create an account: say that the account is virtual.
mkdir -p "$DEST/usr/local/sbin"
cp "$ROOT/scripts/prefix/virtual-user" "$DEST/usr/local/sbin/useradd"
cp "$ROOT/scripts/prefix/virtual-user" "$DEST/usr/local/sbin/adduser"
chmod 755 "$DEST/usr/local/sbin/useradd" "$DEST/usr/local/sbin/adduser"

# ---- 3. the second stage, under dn-init
say "debootstrap, second stage (under dn-init)"
# shellcheck disable=SC2086
if ! env -i HOME=/root TERM=dumb PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 \
     "$DEST/usr/lib/deb-native/dn-init" -q $DN_TINY_FLAGS --initial root -t "$DEST" -- /bin/sh /debootstrap/debootstrap --second-stage; then
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
if [ "$DISTRO" = ubuntu ]; then
  # Ubuntu's -updates and -security are on the same mirror; the keyring is the
  # tree's own (ubuntu-keyring), which the base system installs.
  KEYLINE=
  if [ -f "$DEST/usr/share/keyrings/ubuntu-archive-keyring.gpg" ]; then
    KEYLINE="Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg"
  fi
  {
    printf 'Types: deb\nURIs: %s\nSuites: %s %s-updates\nComponents: main\n' "$MIRROR" "$SUITE" "$SUITE"
    if [ -n "$KEYLINE" ]; then printf '%s\n' "$KEYLINE"; fi
    printf '\nTypes: deb\nURIs: %s\nSuites: %s-security\nComponents: main\n' "$MIRROR" "$SUITE"
    if [ -n "$KEYLINE" ]; then printf '%s\n' "$KEYLINE"; fi
  } > "$DEST/etc/apt/sources.list.d/ubuntu.sources"
else
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
fi
rm -f "$DEST/etc/apt/sources.list"

run() { "$DEST/boot/boot.sh" "$@"; }

say "apt update"
run apt-get update

# The shim is not built here: it is a runtime artifact, linked against a glibc,
# and build-shim.sh builds it once for every tree (--in-prefix or --cross).  A
# prefix copies it in step 2 like the rest, so this build needs no compiler in
# the tree -- a minimal prefix has none.

if [ -n "$(echo $SET_PKGS $PACKAGES)" ]; then
  say "packages: $(echo $SET_PKGS $PACKAGES)"
  # shellcheck disable=SC2086
  run apt-get install -y $SET_PKGS $PACKAGES
fi
run apt-get clean

# ---- the host profile: the runtime the prefix boots
if [ "$HOST" = adb ]; then
  # lane.c has one host-dependent site, so the adb prefix needs its own shim:
  # scripts/build-shim.sh --host adb into $RUNTIME_ADB.
  [ -f "$RUNTIME_ADB/dn-lane.so" ] || die "an adb prefix needs a shim built with --host adb (scripts/build-shim.sh --cross --host adb)"
  cp "$RUNTIME_ADB/dn-init" "$RUNTIME_ADB/dn-lane.so" "$DEST/usr/lib/deb-native/"
  chmod 755 "$DEST/usr/lib/deb-native/dn-init"
  printf '/sdcard\t/storage/emulated/0\tro\n' >> "$DEST/boot/binds"
fi

# A tar extraction by the host user drops the setuid bits; record the list so
# init.sh can restore them on every boot (the app, Termux and ADB hosts alike).
( cd "$DEST" && find . -xdev -type f -perm -4000 -printf '%P\n' | sort ) > "$DEST/boot/setuid"

# ---- what the prefix is
VER=$(cat "$ROOT/VERSION")
cat > "$DEST/boot/version" <<V
# deb-native prefix (docs/building.md)
format=2
name=$NAME
desc=$DISTRO_NAME $SUITE ($VARIANT, set $SET) for deb-native $VER
version=$VER
arch=aarch64
host=$HOST
suite=$SUITE
shim=1
dnexec=1
shim_cancel=$( sed -n 's/^cancel=//p' "$RUNTIME/dn-lane.so.manifest" 2>/dev/null || echo unknown )
shim_libc_sha256=$( sed -n 's/^libc_sha256=//p' "$RUNTIME/dn-lane.so.manifest" 2>/dev/null || echo unknown )
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
size=$(du -sm "$DEST" | cut -f1)
V

say "done: $DEST ($(du -sh "$DEST" | cut -f1))"
echo "boot it:    $DEST/boot/boot.sh"
echo "run a command:  $DEST/boot/boot.sh dpkg -l"
