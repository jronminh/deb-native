#!/bin/sh
# Build core-deb on top of a core-ultra tree (docs/spec/prefix-layers.md).
#
# Build stage only. The build host is separate from every target and has full
# toolchain resources (a CI runner or a Debian arm64 machine with gcc): nothing
# here runs inside a prefix or depends on Termux or on a running core-ultra.
# The .debs are fetched here, and only their translated result goes into the
# tarball. Installing core-deb needs no network; a later `apt update` does.
#
# Build host tools: dpkg-deb, wget, xz, sha256sum, awk, sh, and the overlay's
# own dn-elf (from DN_OVERLAY) to translate interpreters.
#
# Inputs:
#   BASE          a core-ultra tree in build form (cut-core-ultra.py output,
#                 before pack-prefix.py), with its .dn/packages list.
#   DN_GLIBC_PREFIX a deb-native prefix whose glibc is the Android-patched
#                 build (its loader reports "GNU libc"). Its 10 patched files
#                 replace the ones in Debian's libc6/libc-bin (step 2).
#   DN_OVERLAY    the glibc overlay (build-overlay-glibc.sh output): dn-shim.so,
#                 dn-run, dn-trace, dn-elf.
#   DEB_MIRROR    Debian mirror, default http://deb.debian.org/debian
#   DEB_SUITE     default trixie
#   DEB_CACHE     downloaded .debs, kept across builds
#                 (default ~/.cache/deb-native/debs); a cached file is
#                 only checked against its sha256, never downloaded again
#   PREFIX_ROOT   the absolute path the artifact's files will name (the
#                 loader path), e.g. /data/data/org.dn.shell/files/core-deb
#   DEB_LIST      the pinned package list to install on top of BASE, one
#                 `name ver arch` per line (a .dn/packages file). Required.
#   DN_PROFILE    optional: a file of package names written as .dn/profile
#                 (the packages the prefix restores from the mirror)
#   STAGE_OUT     optional: copy the built tree here before packaging, so a
#                 caller can cut core-ultra from it (cut-core-ultra.py)
#   OUT           output tarball
#
# Usage: DN_GLIBC_PREFIX=... DN_OVERLAY=... PREFIX_ROOT=... DEB_LIST=... \
#        build-core-deb.sh BASE OUT.tar.gz
set -eu
BASE=${1:?usage: build-core-deb.sh BASE OUT.tar.gz}
OUT=${2:?usage: build-core-deb.sh BASE OUT.tar.gz}
: "${DN_GLIBC_PREFIX:?set DN_GLIBC_PREFIX to a deb-native prefix whose glibc is the patched build}"
: "${DN_OVERLAY:?set DN_OVERLAY to the build-overlay-glibc.sh output dir}"
: "${PREFIX_ROOT:?set PREFIX_ROOT to the absolute build path of the artifact}"
: "${DEB_LIST:?set DEB_LIST to the pinned package list (.dn/packages format)}"
MIRROR=${DEB_MIRROR:-http://deb.debian.org/debian}
SUITE=${DEB_SUITE:-trixie}
SECURITY=${DEB_SECURITY:-http://security.debian.org/debian-security}
STAGE_OUT=${STAGE_OUT:-}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
LOADER=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1

die() { echo "build-core-deb: $*" >&2; exit 1; }
for t in dpkg-deb wget xz; do command -v "$t" >/dev/null 2>&1 || die "$t not found"; done
ELF=$DN_OVERLAY/dn-elf
[ -x "$ELF" ] || die "no dn-elf in DN_OVERLAY ($ELF)"
[ -d "$BASE" ] || die "BASE is not a directory: $BASE"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
STAGE=$W/stage
CACHE=${DEB_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/deb-native/debs}
mkdir -p "$STAGE" "$CACHE"  # downloads persist across builds
cp -a "$BASE/." "$STAGE/"

# 1. Pinned package list -> download from the mirror, verify sha256.
#    The Packages index for the suite gives the pool path and checksum.
# Pinned versions come from three archives: the release, its updates and the
# security archive. Index every one, and take a package from the first that has
# the exact version.
: > "$W/index.tsv"
for base in "$MIRROR/dists/$SUITE/main" "$MIRROR/dists/$SUITE-updates/main" \
            "$SECURITY/dists/$SUITE-security/main"; do
  if wget -q -O "$W/Packages.xz" "$base/binary-arm64/Packages.xz"; then
    xz -dc "$W/Packages.xz" | awk -v OFS='\t' '
      /^Package: / { p = substr($0, 10) }
      /^Version: / { v = substr($0, 10) }
      /^Filename: / { f = substr($0, 11) }
      /^SHA256: / { s = substr($0, 9) }
      /^$/ { if (p != "") print p, v, f, s; p = v = f = s = "" }
    ' >> "$W/index.tsv"
    echo "build-core-deb: indexed $base"
  else
    echo "build-core-deb: no index at $base (skipped)" >&2
  fi
done
while read -r name ver _arch; do
  [ -n "$name" ] || continue
  line=$(awk -F '\t' -v n="$name" -v v="$ver" '$1 == n && $2 == v { print; exit }' "$W/index.tsv")
  [ -n "$line" ] || die "$name $ver is not in any indexed archive"
  f=$(printf '%s' "$line" | cut -f3)
  s=$(printf '%s' "$line" | cut -f4)
  deb=$CACHE/$(basename "$f")
  if [ ! -f "$deb" ]; then
    wget -q -O "$deb" "$MIRROR/$f" || wget -q -O "$deb" "$SECURITY/$f" || die "cannot fetch $name $ver"
  fi
  got=$(sha256sum "$deb" | cut -d' ' -f1)
  [ "$got" = "$s" ] || die "$name $ver: sha256 mismatch"
  echo "$name $ver $deb"
done < "$DEB_LIST" > "$W/debs.list"
echo "build-core-deb: $(wc -l < "$W/debs.list") packages verified"

# 2. Glibc, the way the build always did it (docs/spec/dn-glibc-prefix.md): Debian's
#    own libc6 and libc-bin, extracted like every package in step 3, then the
#    10 files the Android patch changes overwritten with the patched build from
#    a deb-native prefix (DN_GLIBC_PREFIX). The patched files are the only glibc
#    not from Debian; the loader must say "GNU libc" or the script stops.
PATCHED="usr/lib/aarch64-linux-gnu/libc.so.6
usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
usr/lib/aarch64-linux-gnu/libresolv.so.2
usr/lib/aarch64-linux-gnu/libnsl.so.1
usr/lib/aarch64-linux-gnu/libnss_compat.so.2
usr/lib/aarch64-linux-gnu/libnss_hesiod.so.2
usr/lib/aarch64-linux-gnu/librt.so.1
usr/sbin/ldconfig
usr/bin/localedef
usr/bin/iconv"
"$DN_GLIBC_PREFIX/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" --version 2>/dev/null | head -1 | grep -q "GNU libc" \
  || die "DN_GLIBC_PREFIX's loader is not the patched build (it must say GNU libc)"
echo "$PATCHED" | while IFS= read -r rel; do
  [ -e "$DN_GLIBC_PREFIX/$rel" ] || die "patched file missing: $DN_GLIBC_PREFIX/$rel"
done

# 3. Every package: extract the files into the stage (glibc included, from
#    Debian's pinned debs).
while read -r name ver deb; do
  dpkg-deb -x "$deb" "$STAGE"
done < "$W/debs.list"

# 2b. Overwrite the 10 patched files.
echo "$PATCHED" | while IFS= read -r rel; do
  mkdir -p "$STAGE/$(dirname "$rel")"
  cp -f "$DN_GLIBC_PREFIX/$rel" "$STAGE/$rel"
done

# 4. Translate: every glibc ELF whose interpreter is a Debian loader gets the
#    prefix's own loader (dn-elf grows the PT_LOAD if the path is longer);
#    pack-prefix.py then gives it the 256-byte capacity.
#    Bionic binaries keep their /system/bin/linker64.
find "$STAGE" -type f | while IFS= read -r f; do
  i=$("$ELF" get-interp "$f" 2>/dev/null) || continue
  case $i in
    */ld-linux-aarch64.so.1) "$ELF" set-interp "$f" "$PREFIX_ROOT/$LOADER" 256 ;;
  esac
done

# 5. dpkg's database for every package in the artifact: status and file lists,
#    from the packages' own control and contents, so apt sees them installed
#    and never replaces the patched glibc.
mkdir -p "$STAGE/var/lib/dpkg/info" "$STAGE/var/lib/dpkg/updates"
: > "$STAGE/var/lib/dpkg/status"
while read -r name ver deb; do
  dpkg-deb -f "$deb" >> "$STAGE/var/lib/dpkg/status"
  case " libc6 libc-bin " in
    *" $name "*) printf 'Status: hold ok installed\n\n' >> "$STAGE/var/lib/dpkg/status" ;;
    *) printf 'Status: install ok installed\n\n' >> "$STAGE/var/lib/dpkg/status" ;;
  esac
  dpkg-deb -c "$deb" | awk '{ p = $6; sub(/^\.\//, "", p); if (p != "" && p !~ /\/$/) print "/" p }' \
    > "$STAGE/var/lib/dpkg/info/$name.list"
done < "$W/debs.list"

# 5b. Library search. The patched glibc is built for a flat libdir (usr/lib, as
#     its path.prefix=/usr implies); the packages put libraries in the Debian
#     multiarch directory. Relative links in usr/lib make both work without an
#     ld.so.cache (prefix-contract.md, build invariants).
for d in "$STAGE/usr/lib/aarch64-linux-gnu"/*.so*; do
  [ -e "$d" ] || [ -L "$d" ] || continue
  n=$(basename "$d")
  [ -e "$STAGE/usr/lib/$n" ] || [ -L "$STAGE/usr/lib/$n" ] || ln -s "aarch64-linux-gnu/$n" "$STAGE/usr/lib/$n"
done

# 5a. Files of Debian packages this artifact does not ship must not be on disk:
#     the restore installs those packages, and dpkg refuses to overwrite a file
#     that no package owns. Such files come from the base tree (library closure)
#     or are left from a trimmed package. Remove them; directories stay.
ship_files=$W/owned-ship.txt; all_files=$W/owned-all.txt; : > "$ship_files"; : > "$all_files"
for d in "$CACHE"/*.deb; do
  n=$(dpkg-deb -f "$d" Package)
  dpkg-deb -c "$d" | awk '{ f = $6; sub(/^\.\//, "", f); if (f != "" && f !~ /\/$/) print f }' > "$W/f.txt"
  cat "$W/f.txt" >> "$all_files"
  if cut -d' ' -f3 "$W/debs.list" | grep -qx "$d"; then cat "$W/f.txt" >> "$ship_files"; fi
done
sort -u "$ship_files" > "$W/ship.sorted"; sort -u "$all_files" > "$W/all.sorted"
comm -23 "$W/all.sorted" "$W/ship.sorted" > "$W/orphans.txt"
pruned=0
while IFS= read -r f; do
  if [ -f "$STAGE/$f" ] || [ -L "$STAGE/$f" ]; then rm -f "$STAGE/$f"; pruned=$((pruned + 1)); fi
done < "$W/orphans.txt"
echo "build-core-deb: pruned $pruned files of packages not shipped"

# 6. Overlay: the glibc-built runtime.
mkdir -p "$STAGE/usr/lib/deb-native"
for f in dn-shim.so dn-run dn-trace dn-elf; do
  [ -f "$DN_OVERLAY/$f" ] || die "missing $DN_OVERLAY/$f"
  cp -f "$DN_OVERLAY/$f" "$STAGE/usr/lib/deb-native/$f"
done
# The overlay is built against the build host's loader, and it is copied after the
# translation step: point its interpreter at this prefix's loader here.
for f in usr/lib/deb-native/dn-run usr/lib/deb-native/dn-trace usr/lib/deb-native/dn-elf; do
  "$ELF" set-interp "$STAGE/$f" "$PREFIX_ROOT/$LOADER" 256
done


# 7. deb-native's own layer, which the prefix needs to translate what apt
#    installs later: the apt hooks and their scripts, the launcher scripts,
#    and the stash of the patched glibc files that dn-fix-glibc restores.
#    The apt configuration names the hooks by their installed path.
sh "$ROOT/scripts/host/install-hooks.sh" "$STAGE"
mkdir -p "$STAGE/usr/lib/deb-native/scripts/runtime"
cp -f "$ROOT/scripts/host/bootstrap-prefix.sh" "$STAGE/usr/lib/deb-native/scripts/runtime/"
# The alternatives that mawk's configure step would make: the package manager's
# and the hooks' awk is the link, not the file (a shipped tree is not configured).
[ -e "$STAGE/usr/bin/awk" ] || [ -L "$STAGE/usr/bin/awk" ] || ln -s mawk "$STAGE/usr/bin/awk"
GS="$STAGE/usr/lib/deb-native/glibc-swap"
mkdir -p "$GS"
echo "$PATCHED" | while IFS= read -r rel; do
  mkdir -p "$GS/$(dirname "$rel")"
  cp -f "$DN_GLIBC_PREFIX/$rel" "$GS/$rel"
done

# The loader configuration the overlay needs. With a package-derived BASE these
# may be absent, so the build writes them (the paths are baked and relocated
# with the rest by install.sh).
mkdir -p "$STAGE/etc" "$STAGE/usr/etc/ld.so.conf.d"
[ -e "$STAGE/etc/ld.so.preload" ] || \
  printf '%s\n' "$PREFIX_ROOT/usr/lib/deb-native/dn-shim.so" > "$STAGE/etc/ld.so.preload"
[ -e "$STAGE/usr/etc/ld.so.conf" ] || \
  printf 'include %s/usr/etc/ld.so.conf.d/*.conf\n' "$PREFIX_ROOT" > "$STAGE/usr/etc/ld.so.conf"
[ -e "$STAGE/usr/etc/ld.so.conf.d/dn.conf" ] || \
  printf '%s/usr/lib/aarch64-linux-gnu\n%s/usr/lib\n' "$PREFIX_ROOT" "$PREFIX_ROOT" > "$STAGE/usr/etc/ld.so.conf.d/dn.conf"
HK=$PREFIX_ROOT/usr/lib/deb-native/scripts/install
mkdir -p "$STAGE/etc/apt/apt.conf.d"
cat > "$STAGE/etc/apt/apt.conf.d/50deb-native" <<CONF
DPkg::Pre-Install-Pkgs { "$HK/dn-hook-pre.sh"; };
DPkg::Tools::Options::$HK/dn-hook-pre.sh "";
DPkg::Tools::Options::$HK/dn-hook-pre.sh::Version "3";
DPkg::Post-Invoke { "$HK/dn-hook-post.sh"; };
# apt drops to user _apt for its methods; the prefix is one user (fake root),
# where that switch is refused. Run the methods as the current user.
APT::Sandbox::User "root";
# apt's own directories as absolute prefix paths. When apt hands dpkg the .debs
# it links them and the kernel resolves the target's /var/, /etc/ literally --
# the shim rewrites only the calls apt makes, not kernel path resolution -- so
# a "/var/cache/apt/archives/..." target dangles and dpkg fails with "cannot
# stat". Keep apt's cache under $PREFIX_ROOT so the targets are real.
Dir::Cache::archives "$PREFIX_ROOT/var/cache/apt/archives";
Dir::State::lists "$PREFIX_ROOT/var/lib/apt/lists";
CONF
mkdir -p "$STAGE/etc/apt/sources.list.d"
cat > "$STAGE/etc/apt/sources.list.d/debian.sources" <<SRC
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
if [ -n "${DN_PROFILE:-}" ]; then
  mkdir -p "$STAGE/.dn"
  cp -f "$DN_PROFILE" "$STAGE/.dn/profile"
fi

# Keep the built tree when a caller wants to cut core-ultra from it.
if [ -n "$STAGE_OUT" ]; then
  rm -rf "$STAGE_OUT"
  cp -a "$STAGE" "$STAGE_OUT"
  echo "build-core-deb: tree kept in $STAGE_OUT"
fi

sh "$ROOT/scripts/build/package-prefix.sh" "$STAGE" --root "$PREFIX_ROOT" --name core-deb \
  --desc "core-deb: core-ultra plus apt, dpkg and the translation hooks" --out "$OUT"
