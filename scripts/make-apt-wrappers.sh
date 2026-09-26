#!/bin/sh
# Generate architecture-aware wrappers for the standard package commands --
# apt, apt-get, apt-cache, dpkg -- into the prefix's launcher dir (which
# dn-activate.sh puts first on PATH). No new command name.
#
# Routing rule (chosen): **Termux wins.** If Termux provides the package, it
# installs normally through Termux's own apt/dpkg (aarch64, Bionic; the
# pipeline is not involved). Only a package that exists in the Debian arm64
# bank and NOT in Termux is sent to the deb-native prefix, where the apt
# hooks (patch + grun + launchers) apply.
#
# This dispatch has to happen before apt runs: apt/dpkg resolve
# --instdir/--admindir once per invocation, so they cannot send one package
# to Termux's root and another to the prefix in the same run.
#
# Usage: make-apt-wrappers.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-apt-wrappers.sh INSTDIR}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/.." && pwd)
DNPREFIX=$(dirname "$INSTDIR")
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
AC="$DNPREFIX/etc/apt.conf"

[ -d "$LAUNCHDIR" ] || { echo "make-apt-wrappers: run make-launchers.sh first" >&2; exit 1; }

for n in apt apt-get apt-cache; do
cat > "$LAUNCHDIR/$n" <<EOF
#!/system/bin/sh
# deb-native arch-aware $n (generated; do not edit).
# A leaked APT_CONFIG (some installs export it) would hijack the Termux call
# below too, so both would reach Debian; drop it and set it inline for Debian.
unset APT_CONFIG
REAL="$TP/bin/$n"
AC="$AC"
cmd=""
for a in "\$@"; do
  case "\$a" in -*) ;; *) cmd="\$a"; break ;; esac
done
have_tm() { "$TP/bin/apt-cache" show "\$1" >/dev/null 2>&1; }
have_dn() { APT_CONFIG="\$AC" "$TP/bin/apt-cache" show "\$1" >/dev/null 2>&1; }
case "\$cmd" in
  install|reinstall|remove|purge)
    args=""; pkgs=""; seen=0
    for a in "\$@"; do
      if [ "\$seen" = 0 ] && [ "\$a" = "\$cmd" ]; then seen=1; continue; fi
      case "\$a" in
        -*) args="\$args \$a" ;;
        *)  if [ "\$seen" = 1 ]; then pkgs="\$pkgs \$a"; else args="\$args \$a"; fi ;;
      esac
    done
    tm=""; dn=""
    for p in \$pkgs; do
      if have_tm "\$p"; then tm="\$tm \$p"
      elif have_dn "\$p"; then dn="\$dn \$p"
      else tm="\$tm \$p"; fi
    done
    rc=0
    [ -n "\$tm" ] && { "\$REAL" \$cmd \$args \$tm; rc=\$?; }
    [ -n "\$dn" ] && { APT_CONFIG="\$AC" "\$REAL" \$cmd \$args \$dn; rc=\$?; }
    exit \$rc ;;
  update|upgrade|dist-upgrade|full-upgrade)
    "\$REAL" "\$@" || true
    APT_CONFIG="\$AC" "\$REAL" "\$@" || true
    exit 0 ;;
  search|show|policy)
    "\$REAL" "\$@" 2>/dev/null
    APT_CONFIG="\$AC" "\$REAL" "\$@" 2>/dev/null
    exit 0 ;;
  *) exec "\$REAL" "\$@" ;;
esac
EOF
chmod 755 "$LAUNCHDIR/$n"
done

cat > "$LAUNCHDIR/dpkg" <<EOF
#!/system/bin/sh
# deb-native arch-aware dpkg (generated; do not edit).
REAL="$TP/bin/dpkg"
unset APT_CONFIG
DN="$DNPREFIX"; ROOT="$INSTDIR"; REPO="$REPO"
arch=aarch64
for f in "\$@"; do
  case "\$f" in
    *.deb) arch=\$("$TP/bin/dpkg-deb" -f "\$f" Architecture 2>/dev/null || echo aarch64) ;;
  esac
done
case "\$arch" in
  arm64)
    debs=""
    for a in "\$@"; do case "\$a" in *.deb) debs="\$debs \$a" ;; esac; done
    "\$REPO/scripts/apt-hook-pre.sh" "\$DN" \$debs
    "\$REAL" --instdir="\$ROOT" --admindir="\$DN/var/lib/dpkg" \\
        --force-not-root --force-script-chrootless --force-architecture "\$@"
    rc=\$?
    "\$REPO/scripts/apt-hook-post.sh" "\$DN" || true
    exit \$rc ;;
  *) exec "\$REAL" "\$@" ;;
esac
EOF
chmod 755 "$LAUNCHDIR/dpkg"

echo "==> arch-aware wrappers in $LAUNCHDIR (apt apt-get apt-cache dpkg)"
