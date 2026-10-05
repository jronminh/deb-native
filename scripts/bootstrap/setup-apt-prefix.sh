#!/bin/sh
# Bootstrap a self-contained prefix (docs/spec/design.md): a small, complete
# Debian root of its own -- its own Debian apt/dpkg (run under the prefix's
# glibc via the shim), this project's own libc6/libc-bin (the dn-glibc fused
# loader, docs/spec/dn-glibc-prefix.md) and Debian base -- so installing into
# it is plain apt, as on Debian. Termux only fetches and drives the
# bootstrap; everything lives under NEWPREFIX and the prefix reads only its
# own config (0.7.0).
#
# Every fresh prefix is dn-glibc, out of the box: the fused loader, not
# ld-dn. DN_GLIBC_DEBS (required) is a directory holding Debian's real
# libc6.deb and libc-bin.deb, the fused dn-shim.so, and a files/
# tree with the 10 patched glibc files (docs/spec/deploy.md) --
# dn-install-glibc.sh's usage comment has the details; how those get built
# and distributed is a separate, still-open question, not this script's.
#
# Built the way debootstrap builds a root: the base first, apt last.
#
#   Stage 0, bootstrap (no prefix apt config yet)
#     1. directories, an empty dpkg database (arm64 foreign), the runtime
#        (shim, dn-shell, dn-run, privilege layer), $NEWPREFIX/root ->
#        Termux's home -- all needed before any package script runs
#     2. a throwaway apt config in a temp dir: Debian index, then this
#        project's own libc6/libc-bin (dn-install-glibc.sh) straight into
#        the prefix
#     3. download the base and its dependencies (apt --download-only)
#     4. translate every .deb in Termux's environment (dn-translate-deb.sh:
#        one unpack/repack each, maintainer scripts included), as a batch
#     5. install with Termux's dpkg: unpack all, then configure all
#        -- every file of the base is on disk before any postinst runs
#        (Debian never declares its Essential tools as dependencies);
#        hold the base
#   Stage 1, package database and apt config: sources, pins, apt.conf with
#     the install hooks, and stage 0's verified, rewritten index
#   Stage 2, front end: launchers, routing wrappers, PATH
#
# Usage: setup-apt-prefix.sh NEWPREFIX [debian-suite (default: stable)]
#
# Signatures: Termux ships no Debian keys, so the very first index download
# is unverified; the bootstrap then fetches debian-archive-keyring, accepts it
# only if it verifies that index with a key whose fingerprint is written
# below (DEBIAN_KEYS), and from then on apt verifies everything. The keyring
# is installed into the prefix, so the prefix's own apt verifies too.
set -eu
umask 022
NEWPREFIX=${1:?usage: setup-apt-prefix.sh NEWPREFIX [suite]}
case "$NEWPREFIX" in /*) ;; *) NEWPREFIX="$PWD/$NEWPREFIX" ;; esac
SUITE=${2:-stable}
# The glibc bundle (DN_GLIBC_DEBS): a dir with libc6.deb, libc-bin.deb,
# dn-shim.so and files/ (dn-install-glibc.sh).  When unset, fetch the
# rolling bundle published by .github/workflows/build-glibc.yml.
if [ -z "${DN_GLIBC_DEBS:-}" ]; then
  DN_BUNDLE_TMP=$(mktemp -d)
  echo "Fetching the glibc bundle from GitHub ..."
  curl -fsSL "${DN_GLIBC_BUNDLE_URL:-https://github.com/jronminh/deb-native/releases/download/glibc-bundle/dn-glibc-bundle.tar.gz}" \
    | tar xz -C "$DN_BUNDLE_TMP"
  DN_GLIBC_DEBS="$DN_BUNDLE_TMP"
fi
case "$DN_GLIBC_DEBS" in /*) ;; *) DN_GLIBC_DEBS="$PWD/$DN_GLIBC_DEBS" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL="$HERE/../install"
RUNTIME="$HERE/../runtime"
DN=$NEWPREFIX
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
DPKG="$TP/bin/dpkg --admindir=$DN/var/lib/dpkg --instdir=$DN --force-not-root --force-script-chrootless"

# The base is translated with the HOST (Termux) patchelf: dn-translate-deb.sh
# runs in Termux's environment here, and the prefix's own patchelf does not
# exist yet. Nothing downstream checked this -- the silent `|| interp=""` in
# dn-translate-deb.sh would leave every base ELF, the prefix's seeded patchelf
# included, on /lib/ld-linux-aarch64.so.1 and install them anyway, and then
# every later runtime apt install would be silently untranslated. Require it
# up front so a missing translator fails before the prefix is touched.
command -v patchelf >/dev/null 2>&1 && patchelf --version >/dev/null 2>&1 || {
  echo "E: the Termux host patchelf is required to translate packages: pkg install patchelf" >&2
  exit 1
}

# The base: what every Debian package assumes is there -- 0.1.x's
# bootstrap-base.sh set plus Debian's Essential tools maintainer scripts call
# by name (without them a script's `sed /etc/x` falls through to Termux's
# Bionic sed, which the shim never reaches). Tools first: unpack order is the
# order preinsts run in.
# 0.7.0 minimal: pure fake-root identity -- base-passwd alone (apt's
# `base-passwd | adduser` alternative is satisfied by base-passwd, and
# apt.postinst creates its `_apt` user with `adduser ... || true`, so the
# user-management/PAM stack is not needed). debconf here has no perl
# dependency; perl is only needed once a perl script actually is.
# file + patchelf are what the run-time translate hook (dn-hook-pre.sh ->
# dn-translate-deb.sh) needs when the prefix's own apt installs a package;
# without them a runtime install is not translated (0.7.0).
TOOLS="mawk coreutils sed grep findutils"
SYSTEM="base-files base-passwd dash bash debianutils diffutils gzip tar"
APT="apt dpkg libapt-pkg7.0 gpgv sqv xz-utils ca-certificates debconf file patchelf"
BASE="$TOOLS $SYSTEM $APT"
# Not held: new Debian releases bring new keys through it.
KEYRING="debian-archive-keyring"

# Trust anchor: the primary fingerprints of Debian's archive keys for the
# current and previous stable release, as shipped in Debian's own
# debian-archive-keyring (usr/share/keyrings/debian-archive-keyring.gpg).
# A new Debian release signs with new keys: add them here.
DEBIAN_KEYS="
04B54C3CDCA79751B16BC6B5225629DF75B188BD
5E04A1E3223A19A20706E20F9904613D4CCE68C6
41587F7DB8C774BCCF131416762F67A0B2C39DE4
B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8
05AB90340C0C5E797F44A8C8254CF3B5AEC0A8F0
4D64FEC119C2029067D6E791F8D2585B8783D481"

# Safety: never inside Termux's own prefix (a one-way transformation of
# Termux itself, not this).
case "$DN" in
  "$TP"|"$TP"/*)
    echo "E: $DN is inside Termux's prefix ($TP); use a separate prefix outside both trees" >&2
    exit 1 ;;
esac

write_sources() {  # FILE [OPTIONS]  (OPTIONS: "trusted=yes" for the one unverified fetch)
  o="arch=arm64${2:+ $2}"
  cat > "$1" <<EOF
deb [$o] https://deb.debian.org/debian $SUITE main contrib non-free-firmware
deb [$o] https://deb.debian.org/debian ${SUITE}-updates main contrib non-free-firmware
deb [$o] https://security.debian.org/debian-security ${SUITE}-security main contrib non-free-firmware
EOF
}
# Debian's own copies of the stand-ins must never install: its libc6 dies
# under Android's seccomp filter, its dpkg/apt would replace the launchers.
# sudo/doas: setuid-root binaries that cannot work here; the names are kept
# for deb-native's own later (TODO.md, "sudo").
#
# libc6-dev/libc-dev-bin are NOT in this list on purpose (found 2026-10-01,
# testing the Alpha goal's "Compilers" item): once libc6 is this project's
# own patched build at Debian's *exact*, unmodified version string
# (dn-package-glibc.sh, no +dnN suffix -- deliberately, for exactly this),
# their `Depends: libc6 (= ...)`/`(>> ...) (<< ...)` is genuinely satisfied
# and Debian's real packages install and work unmodified -- no reason to
# fork them too, which would only cascade into their own dependencies
# (confirmed hitting this trying to patch libc6-dev standalone first).
# libc6/libc-bin are NOT in this pin: docs/log/findings/libc6-dev-gap-closed.md
# hit the exact same solver breakage one package over (libc6-dev pinned -1 ->
# apt refuses it as a candidate at all, "no installation candidate", even
# for its own already-installed version) and the fix there was removing the
# pin. Here dpkg hold (dn-install-glibc.sh's install_held) already does the
# "keep apt upgrade from swapping in Debian's real build" job -- unlike
# libc6-dev at the time of that finding, so there is no need for the
# solver-breaking -1 on top. libc-l10n/locales stay pinned: the same
# exact-version-match reasoning as libc6-dev likely applies, but is not yet
# tested, and neither has a dpkg hold of its own.
write_pins() {  # FILE
  # apt/dpkg are NOT pinned any more: they are Debian's own in the prefix
  # now (0.7.0), not Termux stand-ins.
  PINNED="libc-l10n:arm64 locales:arm64 sudo:arm64 doas:arm64"
  for o in deb.debian.org security.debian.org; do
    printf 'Package: %s\nPin: origin %s\nPin-Priority: -1\n\n' "$PINNED" "$o"
  done > "$1"
}

# === Stage 0: bootstrap ===================================================
# Stage and progress markers for install.sh's terminal display (it shows
# only these; everything else goes to its log). Nothing when run directly.
mark() { [ -z "${DN_INSTALL_LOG:-}" ] || echo "::$*"; }

echo "Bootstrapping the Debian base into $DN ..."

# 1. Directories, database, runtime, /root. Only the usr/ side is created:
# base-files ships bin, lib, sbin as links to usr/*, and its preinst refuses
# if they already exist as directories.
mkdir -p "$DN/var/lib/dpkg/updates" "$DN/var/lib/dpkg/info" "$DN/var/log" \
         "$DN/var/lib/deb-native" "$DN/usr/bin" "$DN/usr/lib" "$DN/tmp" "$DN/run"
[ -f "$DN/var/lib/dpkg/status" ] || : > "$DN/var/lib/dpkg/status"
[ -f "$DN/var/lib/dpkg/available" ] || : > "$DN/var/lib/dpkg/available"
"$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --print-foreign-architectures | grep -qx arm64 \
  || "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --add-architecture arm64
mark stage "building the runtime (shim, dn-shell, dn-run)"
"$INSTALL/setup-runtime.sh" "$DN"
# base-passwd's only user is root, home /root, and maintainer scripts write
# there: make it Termux's home (the shim rewrites /root into the prefix).
if [ ! -e "$DN/root" ] && [ ! -L "$DN/root" ]; then
  ln -s "$HOME" "$DN/root"
elif [ ! -L "$DN/root" ]; then
  echo "W: $DN/root exists and is not a link; leaving it" >&2
fi
# Real glibc programs' NSS (libnss_dns) reads /etc/resolv.conf for
# nameservers -- Termux's own Bionic resolver doesn't need it, so nothing
# populated it until this project's first real glibc network client hit
# "Temporary failure resolving" with an empty prefix /etc (found 2026-10-01
# trying real apt/dpkg, TODO.md). A REAL file, not a symlink into Termux's
# tree, so the prefix keeps working with Termux's tree gone (0.7.0 runtime
# independence): snapshot Termux's resolver at install, else public resolvers.
mkdir -p "$DN/etc"
if [ ! -e "$DN/etc/resolv.conf" ] && [ ! -L "$DN/etc/resolv.conf" ]; then
  if [ -r "$TP/etc/resolv.conf" ]; then
    cp "$TP/etc/resolv.conf" "$DN/etc/resolv.conf"
  else
    printf 'nameserver 8.8.8.8\nnameserver 8.8.4.4\n' > "$DN/etc/resolv.conf"
  fi
fi
# A default hosts file: none of the minimal seed ships /etc/hosts, so
# `getent hosts localhost` would fail (0.7.0).
if [ ! -e "$DN/etc/hosts" ]; then
  printf '127.0.0.1\tlocalhost\n::1\t\tlocalhost ip6-localhost ip6-loopback\n' > "$DN/etc/hosts"
fi

# 2. Throwaway apt config: resolves and downloads, never installs. Its status
# file is the prefix's, so the stand-ins count as installed.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/lists/partial" "$T/cache" "$T/debs/partial" "$T/keyring/partial" \
         "$T/etc/apt.conf.d" "$T/etc/trusted.gpg.d" "$T/log"
write_sources "$T/sources.list" trusted=yes
write_pins "$T/prefs"
cat > "$T/apt.conf" <<EOF
Dir::State "$T";
Dir::State::lists "$T/lists";
Dir::State::status "$DN/var/lib/dpkg/status";
Dir::Cache "$T/cache";
Dir::Cache::archives "$T/debs";
Dir::Log "$T/log";
Dir::Etc "$T/etc";
Dir::Etc::sourcelist "$T/sources.list";
Dir::Etc::sourceparts "$T/etc";
Dir::Etc::preferences "$T/prefs";
Dir::Etc::preferencesparts "$T/etc";
APT::Architectures { "aarch64"; "arm64"; };
APT::Install-Recommends "false";
Acquire::PDiffs "false";
Acquire::Languages "none";
EOF
TAPT="env APT_CONFIG=$T/apt.conf $TP/bin/apt-get"
mark stage "fetching the Debian index"
$TAPT update

# Signatures. The index above came in unverified (Termux has no Debian keys):
# fetch Debian's keyring package, and accept it only if every InRelease
# verifies with it (gpgv) under a key whose primary fingerprint is one of
# DEBIAN_KEYS. Then switch the temp config to verified sources and update
# again: from here on apt checks every signature and hash itself.
mark stage "verifying Debian's signatures"
$TAPT install -y --download-only -o Dir::Cache::archives="$T/keyring" "$KEYRING"
dpkg-deb -x "$T"/keyring/${KEYRING}_*.deb "$T/keyring/x"
# apt ignores (and does not keep) an InRelease it cannot verify, so the
# check fetches the three itself.
mkdir -p "$T/verify"
for d in "deb.debian.org/debian/dists/$SUITE" "deb.debian.org/debian/dists/${SUITE}-updates" \
         "security.debian.org/debian-security/dists/${SUITE}-security"; do
  curl -fsSL -o "$T/verify/$(echo "$d" | tr / _)_InRelease" "https://$d/InRelease"
done
for rel in "$T"/verify/*_InRelease; do
  st=$(gpgv --status-fd 1 --keyring "$T/keyring/x/usr/share/keyrings/debian-archive-keyring.gpg" "$rel" 2>&1) || true
  good=""
  for fpr in $(printf '%s\n' "$st" | awk '/VALIDSIG/ {print $NF}'); do
    case "$DEBIAN_KEYS" in *"$fpr"*) good=$fpr ;; esac
  done
  if [ -z "$good" ] || printf '%s\n' "$st" | grep -q 'BADSIG'; then
    echo "E: ${rel##*/} is not signed by a known Debian archive key" >&2
    printf '%s\n' "$st" >&2
    exit 1
  fi
  echo "Verified ${rel##*/} (Debian key $good)"
done
cp "$T"/keyring/x/etc/apt/trusted.gpg.d/*.asc "$T/etc/trusted.gpg.d/"
write_sources "$T/sources.list"
$TAPT update
# Rewrite Architecture: all -> arm64 once; stage 1 reuses these verified,
# rewritten lists instead of downloading and rewriting them again.
"$INSTALL/dn-debian-index.sh" "$T/lists"
mark stage "installing this project's own libc6/libc-bin (dn-glibc)"
"$HERE/dn-install-glibc.sh" "$DN" "$DN_GLIBC_DEBS"

# 3. Download the base and its dependencies.
#
# Don't trust apt's own solver to *add* packages at this stage: the status
# file has almost nothing in it yet (libc6/libc-bin just forced in ahead of
# their own Depends, docs/spec/dn-glibc-prefix.md "Bootstrap note" --
# dpkg --force-depends configures them, but apt still correctly sees
# libgcc-s1 as genuinely missing), and in that state a plain
# `apt-get install $BASE` refuses to auto-add even a plain, satisfiable
# Depends one level down (confirmed on fe2: `apt-get install libgcc-s1`
# alone reports its own Depends: gcc-14-base "not going to be installed",
# while naming both explicitly resolves fine). So compute the full
# transitive closure ourselves with `apt-cache depends --recurse` (the
# debootstrap technique) and hand apt the complete, explicit list -- it
# only needs to download then, not decide what's needed.
mark stage "resolving the base's full dependency closure"
BASE_CLOSURE=$(APT_CONFIG="$T/apt.conf" apt-cache depends --recurse \
  --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
  --no-enhances -i $BASE $KEYRING 2>/dev/null \
  | grep -v '^ ' | grep -v '^<' | sort -u)
# apt-cache depends --recurse expands every alternative, pulling packages the
# seed already satisfies another way (0.7.0 minimal prefix):
#   base-passwd | adduser            -> base-passwd (drop the user/PAM chain)
#   mawk | gawk | original-awk       -> mawk
#   debconf | debconf-2.0            -> debconf (drop cdebconf + its libs)
# perl is not a dependency of debconf here, and pure fake-root needs no
# user-management/PAM stack, so those go too.
SKIP_RE='^(adduser|passwd|libpam0g|libpam-modules|libpam-modules-bin|perl|perl-base|perl-modules-5.40|libperl5.40|gawk|original-awk|cdebconf|libnewt0.52|libslang2|libtextwrap1|libdebian-installer4)$'
BASE_CLOSURE=$(printf '%s\n' "$BASE_CLOSURE" | grep -vE "$SKIP_RE" || true)
mark stage "downloading the base"
# The total comes from apt's own "N newly installed" line (install.sh reads
# it), not from a separate dry run over the whole index.
mark count Get: auto packages
$TAPT install -y --download-only $BASE_CLOSURE

# apt re-adds deps the seed satisfies another way (SKIP_RE above): drop their
# .debs so they are neither translated nor installed. dpkg --force-depends
# tolerates the missing declarations; apt works without the user/PAM stack
# (sandbox=root; apt.postinst's `adduser ... || true`).
for deb in "$T"/debs/*.deb; do
  case "$(dpkg-deb -f "$deb" Package 2>/dev/null)" in
    adduser|passwd|libpam0g|libpam-modules|libpam-modules-bin|perl|perl-base|perl-modules-5.40|libperl5.40|gawk|original-awk|cdebconf|libnewt0.52|libslang2|libtextwrap1|libdebian-installer4)
      rm -f "$deb" ;;
  esac
done

# 4. Translate, in Termux's environment (no apt hooks involved), several
# packages at once (DN_JOBS, default: the CPU count): each is independent
# -- its own temp dir, its own .deb -- and translation was the largest
# stage of the bootstrap (41 of 97 s on fe2, one at a time).
mark stage "translating packages for the prefix"
J=${DN_JOBS:-$(nproc)}
total=$(ls "$T"/debs/*.deb | wc -l)
{ rc=0
  ls "$T"/debs/*.deb | xargs -P "$J" -I{} sh -c \
    '"$1" "$2" "$3" && echo "::translated $(dpkg-deb -f "$2" Package)"' \
    sh "$INSTALL/dn-translate-deb.sh" {} "$DN" || rc=$?
  echo "$rc" > "$T/translate.rc"; } | {
  n=0
  while IFS= read -r line; do
    case "$line" in
      "::translated "*) n=$((n + 1)); mark progress "$n" "$total" "${line#::translated }" ;;
      *) printf '%s\n' "$line" ;;
    esac
  done; }
[ "$(cat "$T/translate.rc")" = 0 ] || { echo "E: translating the base failed" >&2; exit 1; }

# 5. Install with Termux's dpkg. Unpack order: the helper packages first
# (dpkg ships dpkg-maintscript-helper, debconf ships confmodule -- later
# packages' preinst scripts call them), then libraries, then the tools, then
# everything else (preinsts run at unpack).
pre="" first="" tools="" rest=""
for deb in "$T"/debs/*.deb; do
  p=$(dpkg-deb -f "$deb" Package)
  case "$p" in dpkg|debconf) pre="$pre $deb"; continue ;; esac
  case " $TOOLS " in *" $p "*) tools="$tools $deb"; continue ;; esac
  case "$p" in lib*|zlib*) first="$first $deb" ;; *) rest="$rest $deb" ;; esac
done
mark stage "unpacking"
mark count Unpacking "$total" packages
$DPKG --force-depends --unpack $pre $first $tools $rest
mark stage "configuring"
mark count Setting "$total" packages
$DPKG --configure -a
for p in $TOOLS $SYSTEM; do echo "$p:arm64 hold"; done | "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --set-selections
for p in $TOOLS $SYSTEM; do echo "$p set on hold."; done
# The base is the prefix's own system, not programs for the user's shell:
# make-launchers.sh gives it no launchers, so Termux's ls/sed/grep stay first.
echo $BASE $KEYRING | tr ' ' '\n' > "$DN/var/lib/deb-native/base-packages"
"$INSTALL/dn-fix-alternatives.sh" "$DN"
"$INSTALL/normalize-symlinks.sh" "$DN"

# apt's method sandbox drops to _apt; base-passwd does not ship it and the
# adduser/passwd stack is deliberately not installed (0.7.0 minimal, pure
# fake-root), so create it the way apt.postinst's `adduser --system _apt`
# would -- otherwise apt fails "Could not switch group" while downloading.
grep -q '^_apt:' "$DN/etc/passwd" 2>/dev/null || printf '_apt:x:42:65534::/nonexistent:/usr/sbin/nologin\n' >> "$DN/etc/passwd"
grep -q '^_apt:' "$DN/etc/group" 2>/dev/null || printf '_apt:x:65534:\n' >> "$DN/etc/group"

# === Stage 1: package database and apt config ============================
mark stage "writing the prefix's apt configuration"
echo "Writing the prefix's apt configuration ..."
mkdir -p "$DN/etc/apt/apt.conf.d" "$DN/etc/apt/sources.list.d" \
         "$DN/etc/apt/preferences.d" "$DN/etc/apt/trusted.gpg.d" \
         "$DN/var/cache/apt/archives/partial" "$DN/var/log/apt"
touch "$DN/etc/apt/trusted.gpg"
write_sources "$DN/etc/apt/sources.list"
write_pins "$DN/etc/apt/preferences.d/deb-native"
# Copy the translate/index hooks INSIDE the prefix and point apt.conf at them:
# the runtime hooks must not depend on where the checkout lives (a moved or
# removed checkout used to silently stop translating runtime installs).
"$RUNTIME/install-hooks.sh" "$DN"
HOOKS="$DN/usr/lib/deb-native/scripts/install"
cat > "$DN/etc/apt.conf" <<EOF
// deb-native prefix (generated by scripts/bootstrap/setup-apt-prefix.sh; docs/spec/design.md).
Dir::State "$DN/var/lib/apt";
Dir::State::status "$DN/var/lib/dpkg/status";
Dir::Cache "$DN/var/cache/apt";
Dir::Log "$DN/var/log/apt";
Dir::Etc "$DN/etc/apt";
Dir::Etc::sourcelist "$DN/etc/apt/sources.list";
Dir::Etc::sourceparts "$DN/etc/apt/sources.list.d";
Dir::Etc::trusted "$DN/etc/apt/trusted.gpg";
Dir::Etc::trustedparts "$DN/etc/apt/trusted.gpg.d";
Dir::Etc::preferences "$DN/etc/apt/preferences";
Dir::Etc::preferencesparts "$DN/etc/apt/preferences.d";
// Termux's dpkg is natively "aarch64"; Debian's packages stay arm64, a
// foreign architecture in the prefix's own database.
APT::Architectures { "aarch64"; "arm64"; };
APT::Install-Recommends "false";
Acquire::PDiffs "false";          // the index is rewritten after download
Acquire::Languages "none";
// The prefix is fake-root: apt's default method sandbox drops to _apt with a
// real setgid/setgroups that Android refuses ("Could not switch group"), and
// its seccomp sandbox cannot work under this loader. Run methods as root and
// disable the seccomp sandbox (docs/spec/design.md, 0.7.0).
APT::Sandbox::User "root";
APT::Sandbox::Seccomp "false";
APT::Update::Post-Invoke-Success { "$HOOKS/dn-debian-index.sh $DN/var/lib/apt/lists"; };
Dpkg::Options:: "--instdir=$DN";
Dpkg::Options:: "--admindir=$DN/var/lib/dpkg";
Dpkg::Options:: "--force-not-root";
Dpkg::Options:: "--force-script-chrootless";
// DPkg::Path is the PATH apt gives the dpkg it forks and the maintainer
// scripts that run under it -- apt reads it even though Dir::Bin::dpkg
// itself is not overridden.
// priv/ first: a maintainer script's update-alternatives/dpkg-divert must hit
// the prefix's own wrappers (setup-runtime.sh); then the prefix's own bin
// dirs. No $TP/bin: the bootstrap installs the base with Termux's dpkg
// directly (never apt), so DPkg::Path only takes effect at steady state,
// where the prefix is self-sufficient (0.7.0 R1).
DPkg::Path "$DN/usr/lib/deb-native/priv:$DN/usr/bin:$DN/usr/sbin";
DPkg::Pre-Install-Pkgs { "$HOOKS/dn-hook-pre.sh $DN"; };
DPkg::Tools::Options::$HOOKS/dn-hook-pre.sh "";
DPkg::Tools::Options::$HOOKS/dn-hook-pre.sh::Version "3";
DPkg::Post-Invoke { "$HOOKS/dn-hook-post.sh $DN"; };
EOF
# Debian apt reads its main config from Dir::Etc plus "apt.conf" (i.e.
# /etc/apt/apt.conf -> $DN/etc/apt/apt.conf), NOT from /etc/apt.conf. Expose
# the file there so the prefix's own apt picks up the sandbox/Dir/hook
# settings with no APT_CONFIG in the environment (0.7.0).
ln -sfn ../apt.conf "$DN/etc/apt/apt.conf"
# The index: stage 0's lists, verified and already rewritten -- no second
# download or rewrite. The prefix's next `apt update` refreshes them.
mkdir -p "$DN/var/lib/apt/lists/partial"
cp "$T"/lists/*_Packages "$T"/lists/*Release "$DN/var/lib/apt/lists/"

# === Stage 2: front end ===================================================
mark stage "launchers, routing and the shell interface"
echo "Setting up launchers, routing and the shell interface ..."
"$RUNTIME/make-launchers.sh" "$DN"
"$HERE/../../adapters/deb-native/make-apt-wrappers.sh" "$DN"
"$HERE/../../adapters/deb-native/make-shell-interface.sh" "$DN"
echo "The prefix is ready: apt install <package> (Debian-only names go to $DN)."
