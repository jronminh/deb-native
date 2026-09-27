#!/bin/sh
# Bootstrap a self-contained prefix (docs/design-0.2.0.md): a small, complete
# Debian root of its own -- its own apt/dpkg (Termux's, through stand-in
# launchers), database, libc6 and Debian base -- so installing into it is
# plain apt, as on Debian. Termux is never touched: everything lives under
# NEWPREFIX, and the prefix's apt reads only its own config.
#
# Built the way debootstrap builds a root: the base first, apt last.
#
#   Stage 0, bootstrap (no prefix apt config yet)
#     1. directories, an empty dpkg database (arm64 foreign), the runtime
#        (shim, dn-shell, dn-run, privilege layer), $NEWPREFIX/root ->
#        Termux's home -- all needed before any package script runs
#     2. a throwaway apt config in a temp dir: Debian index, then the
#        stand-ins libc6/dpkg/apt (dn-standins.sh) straight into the prefix
#     3. download the base and its dependencies (apt --download-only)
#     4. translate every .deb in Termux's environment (dn-translate-deb.sh,
#        patch-deb.sh), as a batch
#     5. install with the prefix's own dpkg: unpack all, then configure all
#        -- every file of the base is on disk before any postinst runs
#        (Debian never declares its Essential tools as dependencies);
#        hold the base
#   Stage 1, package database and apt config: sources, pins, apt.conf with
#     the install hooks, apt update (reusing stage 0's download)
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
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DN=$NEWPREFIX
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
DPKG="$TP/bin/dpkg --admindir=$DN/var/lib/dpkg --instdir=$DN --force-not-root --force-script-chrootless"

# The base: what every Debian package assumes is there -- 0.1.x's
# bootstrap-base.sh set plus Debian's Essential tools maintainer scripts call
# by name (without them a script's `sed /etc/x` falls through to Termux's
# Bionic sed, which the shim never reaches). Tools first: unpack order is the
# order preinsts run in.
TOOLS="mawk coreutils sed grep findutils"
SYSTEM="base-files base-passwd dash debianutils diffutils gzip tar hostname ncurses-base ncurses-bin"
CONFIG="debconf cdebconf openssl ca-certificates"
BASE="$TOOLS $SYSTEM $CONFIG"
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

# Safety: never inside Termux's own prefix (that is the naibed branch's
# one-way transformation, not this).
case "$DN" in
  "$TP"|"$TP"/*)
    echo "E: $DN is inside Termux's prefix ($TP); use a separate prefix, e.g. \$HOME/.dn" >&2
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
write_pins() {  # FILE
  PINNED="libc6:arm64 libc-bin:arm64 libc6-dev:arm64 libc-dev-bin:arm64 libc-l10n:arm64 locales:arm64 dpkg:arm64 apt:arm64 sudo:arm64 doas:arm64"
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
         "$DN/var/lib/deb-native" "$DN/usr/bin" "$DN/usr/lib"
[ -f "$DN/var/lib/dpkg/status" ] || : > "$DN/var/lib/dpkg/status"
[ -f "$DN/var/lib/dpkg/available" ] || : > "$DN/var/lib/dpkg/available"
"$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --print-foreign-architectures | grep -qx arm64 \
  || "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --add-architecture arm64
mark stage "building the runtime (shim, dn-shell, dn-run)"
"$HERE/setup-runtime.sh" "$DN"
# base-passwd's only user is root, home /root, and maintainer scripts write
# there: make it Termux's home (the shim rewrites /root into the prefix).
if [ ! -e "$DN/root" ] && [ ! -L "$DN/root" ]; then
  ln -s "$HOME" "$DN/root"
elif [ ! -L "$DN/root" ]; then
  echo "W: $DN/root exists and is not a link; leaving it" >&2
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
# Keep the raw lists for stage 1's apt update (unchanged files are not
# fetched again), then rewrite Architecture: all -> arm64 in the temp copy.
mkdir -p "$DN/var/lib/apt/lists/partial"
cp "$T"/lists/*_Packages "$T"/lists/*Release "$DN/var/lib/apt/lists/" || true
"$HERE/dn-debian-index.sh" "$T/lists"
mark stage "installing the stand-ins libc6, dpkg, apt"
DN_APT_CONFIG="$T/apt.conf" "$HERE/dn-standins.sh" "$DN"

# 3. Download the base and its dependencies.
mark stage "downloading the base"
mark count Get: "$($TAPT -s install $BASE $KEYRING | grep -c '^Inst')" packages
$TAPT install -y --download-only $BASE $KEYRING

# 4. Translate, in Termux's environment (no apt hooks involved).
mark stage "translating packages for the prefix"
n=0; total=$(ls "$T"/debs/*.deb | wc -l)
for deb in "$T"/debs/*.deb; do
  n=$((n + 1))
  mark progress "$n" "$total" "$(dpkg-deb -f "$deb" Package)"
  "$HERE/dn-translate-deb.sh" "$deb" "$DN"
  "$HERE/patch-deb.sh" "$deb" "$DN"
done

# 5. Install with the prefix's dpkg. Unpack order: libraries, then the tools,
# then everything else (preinsts run at unpack).
first="" tools="" rest=""
for deb in "$T"/debs/*.deb; do
  p=$(dpkg-deb -f "$deb" Package)
  case " $TOOLS " in *" $p "*) tools="$tools $deb"; continue ;; esac
  case "$p" in lib*|zlib*) first="$first $deb" ;; *) rest="$rest $deb" ;; esac
done
mark stage "unpacking"
mark count Unpacking "$total" packages
$DPKG --force-depends --unpack $first $tools $rest
mark stage "configuring"
mark count Setting "$total" packages
$DPKG --configure -a
for p in $BASE; do echo "$p:arm64 hold"; done | "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --set-selections
for p in $BASE; do echo "$p set on hold."; done
# The base is the prefix's own system, not programs for the user's shell:
# make-launchers.sh gives it no launchers, so Termux's ls/sed/grep stay first.
echo $BASE $KEYRING | tr ' ' '\n' > "$DN/var/lib/deb-native/base-packages"
"$HERE/dn-fix-alternatives.sh" "$DN"
"$HERE/normalize-symlinks.sh" "$DN"

# === Stage 1: package database and apt config ============================
mark stage "writing the prefix's apt configuration"
echo "Writing the prefix's apt configuration ..."
mkdir -p "$DN/etc/apt/apt.conf.d" "$DN/etc/apt/sources.list.d" \
         "$DN/etc/apt/preferences.d" "$DN/etc/apt/trusted.gpg.d" \
         "$DN/var/cache/apt/archives/partial" "$DN/var/log/apt"
touch "$DN/etc/apt/trusted.gpg"
write_sources "$DN/etc/apt/sources.list"
write_pins "$DN/etc/apt/preferences.d/deb-native"
cat > "$DN/etc/apt.conf" <<EOF
// deb-native prefix (generated by scripts/setup-apt-prefix.sh; docs/design-0.2.0.md).
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
APT::Update::Post-Invoke-Success { "$HERE/dn-debian-index.sh $DN/var/lib/apt/lists"; };
Dpkg::Options:: "--instdir=$DN";
Dpkg::Options:: "--admindir=$DN/var/lib/dpkg";
Dpkg::Options:: "--force-not-root";
Dpkg::Options:: "--force-script-chrootless";
DPkg::Pre-Install-Pkgs { "$HERE/dn-hook-pre.sh $DN"; };
DPkg::Tools::Options::$HERE/dn-hook-pre.sh "";
DPkg::Tools::Options::$HERE/dn-hook-pre.sh::Version "3";
DPkg::Post-Invoke { "$HERE/dn-hook-post.sh $DN"; };
EOF
env APT_CONFIG="$DN/etc/apt.conf" "$TP/bin/apt-get" update

# === Stage 2: front end ===================================================
mark stage "launchers, routing and PATH"
echo "Setting up launchers, routing and PATH ..."
"$HERE/make-launchers.sh" "$DN"
"$HERE/make-apt-wrappers.sh" "$DN"
"$HERE/dn-activate.sh" "$DN"
echo "The prefix is ready: apt install <package> (Debian-only names go to $DN)."
