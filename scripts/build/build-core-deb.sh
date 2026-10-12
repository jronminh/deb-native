#!/bin/sh
# Build core-deb on top of a core-ultra tree (docs/spec/prefix.md).
#
# Build stage only. The build host is separate from every target and has full
# toolchain resources (a CI runner or a Debian arm64 machine with gcc): nothing
# here runs inside a prefix or depends on Termux or on a running core-ultra.
# The .debs are fetched here, and only their translated result goes into the
# tarball. Installing core-deb needs no network; a later `apt update` does.
#
# Build host tools: dpkg-deb, wget or curl, xz, sha256sum, md5sum, awk, sh.
#
# Inputs:
#   BASE          a core-ultra tree in build form (cut-core-ultra.py output,
#                 before pack-prefix.py), with its var/lib/deb-native/packages list.
#   DN_RUNTIME    the runtime runtime (build-trace.sh output):
#                 dn-trace, one self-contained binary (the syscall catalog is
#                 embedded in it).
#   DEB_MIRROR    Debian mirror, default http://deb.debian.org/debian
#   DEB_SUITE     default trixie
#   DEB_CACHE     downloaded .debs, kept across builds
#                 (default ~/.cache/deb-native/debs); a cached file is
#                 only checked against its sha256, never downloaded again
#   PREFIX_ROOT   the absolute path the build assumes for its own tree.  It
#                 is not baked into the artifact (the artifact is relocatable:
#                 dn-trace derives the root from its own location): pack-prefix
#                 takes it only to assert that nothing names it, e.g.
#                 /data/data/org.dn.shell/files/core-deb
#   DEB_LIST      the pinned package list to install on top of BASE, one
#                 `name ver arch` per line (a var/lib/deb-native/packages file). Required.
#   DN_PROFILE    optional: a file of package names written as var/lib/deb-native/profile
#                 (the packages the prefix restores from the mirror)
#   STAGE_OUT     optional: copy the built tree here before packaging, so a
#                 caller can cut core-ultra from it (cut-core-ultra.py)
#   OUT           output tarball
#
# Usage: DN_RUNTIME=... PREFIX_ROOT=... DEB_LIST=... \
#        build-core-deb.sh BASE OUT.tar.gz
set -eu
BASE=${1:?usage: build-core-deb.sh BASE OUT.tar.gz}
OUT=${2:?usage: build-core-deb.sh BASE OUT.tar.gz}
: "${DN_RUNTIME:?set DN_RUNTIME to the build-trace.sh output dir}"
: "${PREFIX_ROOT:?set PREFIX_ROOT to the absolute build path of the artifact}"
: "${DEB_LIST:?set DEB_LIST to the pinned package list (var/lib/deb-native/packages format)}"
MIRROR=${DEB_MIRROR:-http://deb.debian.org/debian}
SUITE=${DEB_SUITE:-trixie}
SECURITY=${DEB_SECURITY:-http://security.debian.org/debian-security}
STAGE_OUT=${STAGE_OUT:-}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
LOADER=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1

die() { echo "build-core-deb: $*" >&2; exit 1; }
for t in dpkg-deb xz md5sum; do command -v "$t" >/dev/null 2>&1 || die "$t not found"; done
command -v wget >/dev/null 2>&1 || command -v curl >/dev/null 2>&1 || die "wget or curl not found"
# fetch URL OUT: wget, or curl on a build host without wget.
fetch() {
  if command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"; else curl -fsSL -o "$2" "$1"; fi
}
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
  if fetch "$base/binary-arm64/Packages.xz" "$W/Packages.xz"; then
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
    # Fetched beside the cache entry and renamed, so a failed download never
    # leaves a partial .deb that later builds would trust.
    fetch "$MIRROR/$f" "$deb.part" || fetch "$SECURITY/$f" "$deb.part" \
      || { rm -f "$deb.part"; die "cannot fetch $name $ver"; }
    mv -f "$deb.part" "$deb"
  fi
  got=$(sha256sum "$deb" | cut -d' ' -f1)
  [ "$got" = "$s" ] || die "$name $ver: sha256 mismatch"
  echo "$name $ver $deb"
done < "$DEB_LIST" > "$W/debs.list"
echo "build-core-deb: $(wc -l < "$W/debs.list") packages verified"

# 2. Glibc is Debian's own stock `libc6`/`libc-bin`, straight from the pinned
#    list (step 1) -- this project patches no glibc. The syscalls Android
#    denies are answered at the syscall boundary by dn-trace, not by a patched
#    glibc (docs/reference/android-platform.md, "Stock `libc6` re-tested").

# 3. Every package: extract the files into the stage (glibc included -- ours).
while read -r name ver deb; do
  dpkg-deb -x "$deb" "$STAGE"
done < "$W/debs.list"

# 4. Interpreters are left as Debian names them.  Runtime v1 does not relocate
#    or repoint PT_INTERP: the tree's root dn-trace sees every exec, and its
#    exec gate runs a glibc-dynamic program through the runtime loader
#    (RT/ld.so) itself, so the kernel never has to resolve a guest PT_INTERP
#    (docs/spec/runtime.md, "The exec gate").

# 5. dpkg's database for every package in the artifact, as dpkg itself would
#    leave it: status (with each package's Conffiles and their md5sums), file
#    lists, and the control files (md5sums, conffiles, maintainer scripts,
#    triggers, shlibs, ...) under info/, named <pkg>:<arch> for a
#    Multi-Arch: same package.  So apt sees them installed and never replaces
#    the patched glibc, dpkg --verify works, conffiles are kept on upgrade and
#    removal runs the package's own scripts.
mkdir -p "$STAGE/var/lib/dpkg/info" "$STAGE/var/lib/dpkg/updates"
: > "$STAGE/var/lib/dpkg/status"
while read -r name ver deb; do
  key=$name
  [ "$(dpkg-deb -f "$deb" Multi-Arch)" = same ] && key="$name:$(dpkg-deb -f "$deb" Architecture)"
  ctl=$W/ctl
  rm -rf "$ctl"
  dpkg-deb -e "$deb" "$ctl"
  {
    dpkg-deb -f "$deb"
    # Unpacked, not configured: the build cannot run a package's scripts,
    # so the prefix's first boot runs them (boot/bootstrap.sh: each
    # preinst, then dpkg --configure -a), as debootstrap's second stage
    # does.  The conffiles are left in place; dpkg takes them as installed.
    # glibc is Debian's own stock package now, so it gets no special status:
    # it configures and upgrades like every other package.
    echo 'Status: install ok unpacked'
    if [ -s "$ctl/conffiles" ]; then
      echo 'Conffiles:'
      while read -r cf; do
        case "$cf" in /*) ;; *) continue ;; esac   # skip remove-on-upgrade etc.
        [ -f "$STAGE$cf" ] || continue
        printf ' %s %s\n' "$cf" "$(md5sum < "$STAGE$cf" | cut -d' ' -f1)"
      done < "$ctl/conffiles"
    fi
    echo
  } >> "$STAGE/var/lib/dpkg/status"
  dpkg-deb -c "$deb" | awk '{ p = $6; sub(/^\.\//, "", p); if (p != "" && p !~ /\/$/) print "/" p }' \
    > "$STAGE/var/lib/dpkg/info/$key.list"
  for f in "$ctl"/*; do
    [ "${f##*/}" = control ] || cp -p "$f" "$STAGE/var/lib/dpkg/info/$key.${f##*/}"
  done
done < "$W/debs.list"
rm -rf "$W/ctl"

# 5b. Library search. Debian's packages put libraries in the Debian multiarch
#     directory; a relative link in usr/lib lets a tree with no ld.so.cache
#     still find them (prefix-contract.md, build invariants).
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

# 6. Runtime: the branch's runtime is three files (docs/spec/monitor.md):
#    dn-monitor, the tree's root process -- the mechanism (the catch/route
#    filter, the ptrace core) with the syscall catalog (src/syscalls.tsv)
#    embedded in it; dn-interp, the *loader launcher* the exec gate's rule 3
#    runs (it builds the environment and execve()s the tree's stock ld.so); and
#    dn-shim.so, the in-process fast path the launcher delivers.
mkdir -p "$STAGE/usr/lib/deb-native" "$STAGE/etc/deb-native"
[ -f "$DN_RUNTIME/dn-monitor" ] || die "missing $DN_RUNTIME/dn-monitor"
[ -f "$DN_RUNTIME/dn-interp" ] || die "missing $DN_RUNTIME/dn-interp"
[ -f "$DN_RUNTIME/dn-interp.conf" ] || die "missing $DN_RUNTIME/dn-interp.conf"
[ -f "$DN_RUNTIME/dn-shim.so" ] || die "missing $DN_RUNTIME/dn-shim.so"
cp -f "$DN_RUNTIME/dn-monitor" "$STAGE/usr/lib/deb-native/dn-monitor"
cp -f "$DN_RUNTIME/dn-interp" "$STAGE/usr/lib/deb-native/dn-interp"
cp -f "$DN_RUNTIME/dn-shim.so" "$STAGE/usr/lib/deb-native/dn-shim.so"
chmod 755 "$STAGE/usr/lib/deb-native/dn-monitor" "$STAGE/usr/lib/deb-native/dn-interp"
# The launcher reads its policy from $DN/etc/deb-native/dn-interp.conf (a
# compiled default stands in when the file is absent).
cp -f "$DN_RUNTIME/dn-interp.conf" "$STAGE/etc/deb-native/dn-interp.conf"
# The prefix's init and the boot script live at the prefix root's boot/
# (docs/spec/prefix.md, "The boot/ directory"): boot.sh is the host's entry by
# convention, init.sh is what dn-monitor runs, install.sh (added by
# pack-prefix) is the host's activation, version is the manifest.
mkdir -p "$STAGE/boot"
cp -f "$ROOT/scripts/prefix/init.sh" "$STAGE/boot/init.sh"
cp -f "$ROOT/scripts/prefix/boot.sh" "$STAGE/boot/boot.sh"
chmod 755 "$STAGE/boot/init.sh" "$STAGE/boot/boot.sh"
# dn-monitor IS the root, so it must run OUTSIDE the tree: it is built static
# (self-contained -- libc and the vendored talloc are linked in), so a poor host
# starts it with a bare exec and no loader at all, and dn-policy never
# translates the root's own path operations.

# 6b. The local repo (docs/spec/runtime.md, "The local repo"): reserved for the
#     packages the runtime cannot cover alone (equivs dummies, a dynamic build
#     standing in for a static program, a package patched to run on Android).
#     It ships empty today -- glibc is Debian's own stock package from the
#     mirror -- so `Packages` is empty and apt finds nothing extra. It lives in
#     RT, outside the dpkg-managed tree. Packages and Release are written by
#     hand, so the build host needs neither dpkg-dev nor apt-utils.
REPO=$STAGE/usr/lib/deb-native/repo
mkdir -p "$REPO"
: > "$REPO/Packages"
gzip -kf "$REPO/Packages"
# Date and the index hashes: without them apt warns ("No Hash entry",
# "Invalid 'Date' entry") and cannot check the indexes it reads.
{
  cat <<REL
Origin: deb-native
Label: deb-native
Suite: stable
Codename: dn
Date: $(date -u -R ${SOURCE_DATE_EPOCH:+-d "@$SOURCE_DATE_EPOCH"})
Architectures: arm64
Description: deb-native local repo
SHA256:
REL
  for i in Packages Packages.gz; do
    printf ' %s %s %s\n' "$(sha256sum < "$REPO/$i" | cut -d' ' -f1)" "$(wc -c < "$REPO/$i" | tr -d ' ')" "$i"
  done
} > "$REPO/Release"


# 7. deb-native's own layer: the bootstrap helper the prefix runs on its first
#    boot (docs/spec/prefix.md, "Boot").  Runtime v1 needs no apt hooks -- a
#    .deb installs intact, and dn-policy rewrites paths and identities at run
#    time (docs/spec/runtime.md).
cp -f "$ROOT/scripts/prefix/bootstrap.sh" "$STAGE/boot/bootstrap.sh"
chmod 755 "$STAGE/boot/bootstrap.sh"
# The alternatives that mawk's configure step would make: the package manager's
# awk is the link, not the file (a shipped tree is not configured).
[ -e "$STAGE/usr/bin/awk" ] || [ -L "$STAGE/usr/bin/awk" ] || ln -s mawk "$STAGE/usr/bin/awk"

# The loader configuration the runtime needs. With a package-derived BASE these
# may be absent, so the build writes them.  Every path is a **guest** path
# (dn-policy maps "/" to the tree): the artifact names no host prefix, so it
# installs anywhere (docs/spec/prefix.md, "Boot").
mkdir -p "$STAGE/etc" "$STAGE/usr/etc/ld.so.conf.d"
[ -e "$STAGE/usr/etc/ld.so.conf" ] || \
  printf 'include /usr/etc/ld.so.conf.d/*.conf\n' > "$STAGE/usr/etc/ld.so.conf"
[ -e "$STAGE/usr/etc/ld.so.conf.d/dn.conf" ] || \
  printf '/usr/lib/aarch64-linux-gnu\n/usr/lib\n' > "$STAGE/usr/etc/ld.so.conf.d/dn.conf"
mkdir -p "$STAGE/etc/apt/apt.conf.d"
cat > "$STAGE/etc/apt/apt.conf.d/50deb-native" <<CONF
# apt's own directories, as guest paths (dn-policy maps "/" to the tree, so
# they resolve to the prefix wherever it is installed).  Explicit, not
# Debian's defaults, so the intent is visible.
Dir::Cache::archives "/var/cache/apt/archives";
Dir::State::lists "/var/lib/apt/lists";
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
# The local repo: the origin pin that keeps the mirror from replacing a
# package the repo carries (none today), docs/spec/runtime.md, "The local
# repo". The path is a guest path: the runtime translates it into the tree.
cat > "$STAGE/etc/apt/sources.list.d/dn-local.list" <<SRC
deb [trusted=yes] file:/usr/lib/deb-native/repo ./
SRC
mkdir -p "$STAGE/etc/apt/preferences.d"
cat > "$STAGE/etc/apt/preferences.d/dn-local" <<CONF
Package: *
Pin: release o=deb-native
Pin-Priority: 1001
CONF
# ca-certificates' postinst writes /etc/ca-certificates.conf through debconf,
# which the tree does not ship (and a shipped tree is not configured): write
# its non-interactive default, every certificate enabled, so
# update-ca-certificates can build the bundle HTTPS needs.
if [ -d "$STAGE/usr/share/ca-certificates" ] && [ ! -e "$STAGE/etc/ca-certificates.conf" ]; then
  ( cd "$STAGE/usr/share/ca-certificates" && find . -type f -name '*.crt' | sed 's|^\./||' | LC_ALL=C sort ) \
    > "$STAGE/etc/ca-certificates.conf"
fi
if [ -n "${DN_PROFILE:-}" ]; then
  mkdir -p "$STAGE/var/lib/deb-native"
  cp -f "$DN_PROFILE" "$STAGE/var/lib/deb-native/profile"
fi

# Keep the built tree when a caller wants to cut core-ultra from it.
if [ -n "$STAGE_OUT" ]; then
  rm -rf "$STAGE_OUT"
  cp -a "$STAGE" "$STAGE_OUT"
  echo "build-core-deb: tree kept in $STAGE_OUT"
fi

sh "$ROOT/scripts/build/package-prefix.sh" "$STAGE" --root "$PREFIX_ROOT" --name core-deb \
  --desc "core-deb: core-ultra plus apt, dpkg and the runtime" --out "$OUT"
