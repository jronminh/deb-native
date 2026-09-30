#!/bin/sh
# Commands for the prefix's launcher dir (on PATH via dn-activate.sh):
#
#   termux-apt, termux-dpkg   Termux's own apt and dpkg, by a name that the
#                             prefix's apt/dpkg can never shadow
#   termux-dn-doctor          the deb-native checks (dn-doctor.sh)
#   dn-shell                  an interactive shell inside the prefix
#   dn-adopt FILE...          run a downloaded glibc program through the prefix
#
# Since 0.2.0 the plain names `apt`, `apt-get`, `apt-cache`, `apt-mark`,
# `dpkg`, `dpkg-query` typed in an interactive shell are the prefix's own
# (aliases, dn-activate.sh), and Termux's packages are managed with `pkg`,
# as Termux recommends. Aliases never reach scripts, so `pkg` and every other
# Termux script still call Termux's real apt/dpkg. The 0.1.x "Termux wins"
# routing wrappers (apt, apt-get, apt-cache, dpkg on PATH) are removed.
#
# Usage: make-apt-wrappers.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-apt-wrappers.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

[ -d "$LAUNCHDIR" ] || { echo "E: no launcher directory (run make-launchers.sh)" >&2; exit 1; }

# 0.1.x routing wrappers: on PATH they would shadow Termux's apt for pkg.
for n in apt apt-get apt-cache dpkg; do
  if [ -f "$LAUNCHDIR/$n" ] && grep -q "deb-native arch-aware" "$LAUNCHDIR/$n"; then
    rm -f "$LAUNCHDIR/$n"
    echo "Removed the 0.1.x routing wrapper $n."
  fi
done

for n in apt dpkg; do
  cat > "$LAUNCHDIR/termux-$n" <<EOF
#!/system/bin/sh
# Termux's own $n (deb-native, generated; do not edit). A leaked APT_CONFIG
# would point it at the prefix: drop it.
unset APT_CONFIG
exec "$TP/bin/$n" "\$@"
EOF
  chmod 755 "$LAUNCHDIR/termux-$n"
done

cat > "$LAUNCHDIR/termux-dn-doctor" <<EOF
#!/system/bin/sh
# deb-native doctor (generated; do not edit).
exec sh "$REPO/scripts/tools/dn-doctor.sh" "$INSTDIR" "\$@"
EOF
chmod 755 "$LAUNCHDIR/termux-dn-doctor"

# dn-shell: an interactive shell inside the prefix -- the maintainer-script
# shell (dn-launch.c: Termux's glibc bash with the path shim, the prefix
# first on PATH), so a script run from it sees Debian's /usr, /etc, /opt.
# For trying direct installers (`curl ... | bash`); `exit` returns to Termux.
ln -sfn "$INSTDIR/usr/bin/dn-shell" "$LAUNCHDIR/dn-shell"

# dn-adopt: make a glibc program obtained outside apt run through the
# prefix (dn-adopt.sh).
cat > "$LAUNCHDIR/dn-adopt" <<EOF
#!/system/bin/sh
# deb-native dn-adopt (generated; do not edit).
exec sh "$REPO/scripts/runtime/dn-adopt.sh" "$INSTDIR" "\$@"
EOF
chmod 755 "$LAUNCHDIR/dn-adopt"

echo "Installed termux-apt, termux-dpkg, termux-dn-doctor, dn-shell and dn-adopt in $LAUNCHDIR."
