#!/bin/sh
# Build core-deb on top of a core-ultra tree (docs/spec/prefix.md).
#
# Build stage only. The build host is separate from every target and has full
# toolchain resources (a CI runner or a Debian arm64 machine with gcc): nothing
# here runs inside a prefix or depends on Termux or on a running core-ultra.
# The .debs are fetched here, and only their translated result goes into the
# tarball. Installing core-deb needs no network; a later `apt update` does.
#
# Build host tools: dpkg-deb, wget, xz, sha256sum, awk, sh.
#
# Inputs:
#   BASE          a core-ultra tree in build form (cut-core-ultra.py output,
#                 before pack-prefix.py), with its .dn/packages list.
#   DN_GLIBC_PREFIX a directory holding the patched `libc6.deb` and
#                 `libc-bin.deb` (built by scripts/glibc/dn-package-glibc.sh
#                 and dn-package-libc-bin.sh; version `<Debian>+dn1`, loader
#                 says "GNU libc"). They replace Debian's in step 2, are
#                 installed in the tree, and ship in the local repo (step 6b).
#   DN_OVERLAY    the runtime overlay (build-overlay-glibc.sh output):
#                 dn-trace and the syscall catalog.
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
: "${DN_GLIBC_PREFIX:?set DN_GLIBC_PREFIX to the dir holding the patched libc6.deb and libc-bin.deb}"
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

# 2. Glibc is shipped as real packages, not swapped files (docs/spec/prefix.md):
#    `libc6` and `libc-bin`, built from the tree's libc6 source with the
#    Android + dn-policy patches, at version `<Debian version>+dn1` so they are
#    visibly not upgradeable from the mirror. They are delivered through the
#    local repo (step 6b); DN_GLIBC_PREFIX is the directory holding the two
#    .debs. Replace the mirror's libc6/libc-bin entries with ours, and check
#    the loader in libc6 says "GNU libc".
for p in libc6 libc-bin; do
  [ -f "$DN_GLIBC_PREFIX/$p.deb" ] || die "missing $DN_GLIBC_PREFIX/$p.deb"
  [ "$(dpkg-deb -f "$DN_GLIBC_PREFIX/$p.deb" Package)" = "$p" ] \
    || die "$DN_GLIBC_PREFIX/$p.deb is not the $p package"
done
while read -r name ver deb; do
  case $name in
    libc6|libc-bin)
      p=$DN_GLIBC_PREFIX/$name.deb
      printf '%s %s %s\n' "$name" "$(dpkg-deb -f "$p" Version)" "$p" ;;
    *) printf '%s %s %s\n' "$name" "$ver" "$deb" ;;
  esac
done < "$W/debs.list" > "$W/debs.list.new"
mv "$W/debs.list.new" "$W/debs.list"
tmpld=$(mktemp -d "$W/ld.XXXXXX")
dpkg-deb -x "$DN_GLIBC_PREFIX/libc6.deb" "$tmpld"
"$tmpld/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" --version 2>/dev/null | head -1 | grep -q "GNU libc" \
  || die "libc6.deb's loader is not the patched build (it must say GNU libc)"
rm -rf "$tmpld"

# 3. Every package: extract the files into the stage (glibc included -- ours).
while read -r name ver deb; do
  dpkg-deb -x "$deb" "$STAGE"
done < "$W/debs.list"

# 4. Interpreters are left as Debian names them.  Runtime v1 does not relocate
#    or repoint PT_INTERP: the tree's root dn-trace sees every exec, and its
#    exec gate runs a glibc-dynamic program through the runtime loader
#    (RT/ld.so) itself, so the kernel never has to resolve a guest PT_INTERP
#    (docs/spec/overlay.md, "The exec gate").

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
# Every deb the prune can see: the cache (older builds' packages) plus the
# ones this build ships -- the patched glibc debs live outside the cache.
ls "$CACHE"/*.deb > "$W/all-debs.txt" 2>/dev/null || :
cut -d' ' -f3 "$W/debs.list" >> "$W/all-debs.txt"
sort -u -o "$W/all-debs.txt" "$W/all-debs.txt"
while IFS= read -r d; do
  [ -f "$d" ] || continue
  dpkg-deb -c "$d" | awk '{ f = $6; sub(/^\.\//, "", f); if (f != "" && f !~ /\/$/) print f }' > "$W/f.txt"
  cat "$W/f.txt" >> "$all_files"
  if cut -d' ' -f3 "$W/debs.list" | grep -qx "$d"; then cat "$W/f.txt" >> "$ship_files"; fi
done < "$W/all-debs.txt"
sort -u "$ship_files" > "$W/ship.sorted"; sort -u "$all_files" > "$W/all.sorted"
comm -23 "$W/all.sorted" "$W/ship.sorted" > "$W/orphans.txt"
pruned=0
while IFS= read -r f; do
  if [ -f "$STAGE/$f" ] || [ -L "$STAGE/$f" ]; then rm -f "$STAGE/$f"; pruned=$((pruned + 1)); fi
done < "$W/orphans.txt"
echo "build-core-deb: pruned $pruned files of packages not shipped"

# 6. Overlay: the runtime -- dn-trace (the tree's root) and the syscall catalog
#    it reads (--syscalls) to build the filter's gate-IP exemption
#    (src/syscalls.tsv, docs/spec/overlay.md).
mkdir -p "$STAGE/usr/lib/deb-native"
for f in dn-trace syscalls.tsv; do
  [ -f "$DN_OVERLAY/$f" ] || die "missing $DN_OVERLAY/$f"
  cp -f "$DN_OVERLAY/$f" "$STAGE/usr/lib/deb-native/$f"
done

# 6b. The local repo (docs/spec/overlay.md, "The local repo"): the patched
#     glibc packages, so apt sees them and the origin pin (below) keeps the
#     mirror from ever replacing them. It lives in RT, outside the
#     dpkg-managed tree. Packages and Release are written by hand, so the
#     build host needs neither dpkg-dev nor apt-utils.
REPO=$STAGE/usr/lib/deb-native/repo
mkdir -p "$REPO"
cp -f "$DN_GLIBC_PREFIX/libc6.deb" "$DN_GLIBC_PREFIX/libc-bin.deb" "$REPO/"
: > "$REPO/Packages"
for d in libc6 libc-bin; do
  {
    dpkg-deb -f "$REPO/$d.deb"
    printf 'Filename: ./%s.deb\n' "$d"
    printf 'Size: %s\n' "$(wc -c < "$REPO/$d.deb")"
    printf 'SHA256: %s\n' "$(sha256sum "$REPO/$d.deb" | cut -d' ' -f1)"
    echo
  } >> "$REPO/Packages"
done
gzip -kf "$REPO/Packages"
cat > "$REPO/Release" <<REL
Origin: deb-native
Label: deb-native
Suite: stable
Codename: dn
Architectures: arm64
Description: deb-native local repo
REL


# 7. deb-native's own layer: the bootstrap helper the prefix runs at login.
#    Runtime v1 needs no apt hooks -- a .deb installs intact, and dn-policy
#    rewrites paths and identities at run time (docs/spec/overlay.md).
mkdir -p "$STAGE/usr/lib/deb-native/scripts/runtime"
cp -f "$ROOT/scripts/host/bootstrap-prefix.sh" "$STAGE/usr/lib/deb-native/scripts/runtime/"
# The alternatives that mawk's configure step would make: the package manager's
# awk is the link, not the file (a shipped tree is not configured).
[ -e "$STAGE/usr/bin/awk" ] || [ -L "$STAGE/usr/bin/awk" ] || ln -s mawk "$STAGE/usr/bin/awk"

# The loader configuration the overlay needs. With a package-derived BASE these
# may be absent, so the build writes them.
mkdir -p "$STAGE/etc" "$STAGE/usr/etc/ld.so.conf.d"
[ -e "$STAGE/usr/etc/ld.so.conf" ] || \
  printf 'include %s/usr/etc/ld.so.conf.d/*.conf\n' "$PREFIX_ROOT" > "$STAGE/usr/etc/ld.so.conf"
[ -e "$STAGE/usr/etc/ld.so.conf.d/dn.conf" ] || \
  printf '%s/usr/lib/aarch64-linux-gnu\n%s/usr/lib\n' "$PREFIX_ROOT" "$PREFIX_ROOT" > "$STAGE/usr/etc/ld.so.conf.d/dn.conf"
mkdir -p "$STAGE/etc/apt/apt.conf.d"
cat > "$STAGE/etc/apt/apt.conf.d/50deb-native" <<CONF
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
# The local repo: the patched glibc, and the origin pin that keeps the mirror
# from replacing it (docs/spec/overlay.md, "The local repo"). The path is a
# guest path: the runtime translates it into the tree.
cat > "$STAGE/etc/apt/sources.list.d/dn-local.list" <<SRC
deb [trusted=yes] file:/usr/lib/deb-native/repo ./
SRC
mkdir -p "$STAGE/etc/apt/preferences.d"
cat > "$STAGE/etc/apt/preferences.d/dn-local" <<CONF
Package: *
Pin: release o=deb-native
Pin-Priority: 1001
CONF
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
  --desc "core-deb: core-ultra plus apt, dpkg and the runtime" --out "$OUT"
