#!/bin/sh
# deb-native doctor. Check (and with --fix, repair) the Termux <-> prefix
# seams that go wrong in practice:
#
#   - a leaked `export APT_CONFIG=...` in the shell rc, which points
#     Termux's own apt (pkg, termux-apt) at the prefix;
#   - a Termux `sources.list` clobbered by installing into $PREFIX;
#   - missing termux-apt/termux-dpkg, 0.1.x routing wrappers left on PATH,
#     or a missing shell interface (~/.termux/shell).
#
# Usage: dn-doctor.sh [PREFIX] [--fix]
#   PREFIX defaults to the deb-native launcher dir found on PATH, else the
#   default prefix beside Termux's usr/ and home/.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

FIX=0
DN_PREFIX=""
for a in "$@"; do
  case "$a" in
    --fix) FIX=1 ;;
    -*) ;;
    *) DN_PREFIX=$a ;;
  esac
done
case "$DN_PREFIX" in ""|/*) ;; *) DN_PREFIX="$PWD/$DN_PREFIX" ;; esac

if [ -z "$DN_PREFIX" ]; then
  old_ifs=$IFS; IFS=:
  for d in $PATH; do
    case "$d" in */usr/lib/deb-native/bin) DN_PREFIX=${d%/usr/lib/deb-native/bin} ;; esac
  done
  IFS=$old_ifs
  [ -n "$DN_PREFIX" ] || DN_PREFIX=$(dirname "$TP")/deb-native
fi

ROOT="$DN_PREFIX"
LAUNCHDIR="$ROOT/usr/lib/deb-native/bin"
fail=0
ok()   { printf '  ok    %s\n' "$1"; }
warn() { printf '  WARN  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

printf 'termux-dn-doctor\n'
printf '  termux prefix:  %s\n' "$TP"
printf '  deb-native:     %s\n' "$DN_PREFIX"

# 1. The prefix must never be Termux's own.
case "$DN_PREFIX" in
  "$TP"|"$TP"/*) bad "prefix is inside Termux's prefix — install into a separate dir (e.g. $(dirname "$TP")/deb-native)" ;;
  *) ok "prefix is separate from Termux" ;;
esac

# 2. A leaked APT_CONFIG in the shell rc hijacks the wrapper's Termux call.
for rc in "$HOME/.bashrc" "$HOME/.profile" "$HOME/.bash_profile"; do
  [ -f "$rc" ] || continue
  if [ -f "$rc" ] && grep -q "APT_CONFIG=" "$rc"; then
    warn "$rc exports APT_CONFIG (makes apt show only Debian)"
    if [ "$FIX" = 1 ]; then
      cp "$rc" "$rc.dn-doctor.bak"
      sed -i "/APT_CONFIG=/d" "$rc"
      ok "removed APT_CONFIG from $rc (backup $rc.dn-doctor.bak)"
    fi
  fi
done
if [ -L "$HOME/.termux/shell" ] && [ -x "$ROOT/usr/bin/dn-shell" ]; then
  ok "shell interface installed (userland is the default session)"
else
  warn "$HOME/.termux/shell missing (run make-shell-interface.sh)"
fi

# 3. Termux's own apt must still point at the Termux repo.
sl="$TP/etc/apt/sources.list"
if [ -f "$sl" ] && grep -q "deb.debian.org\|debian-security" "$sl"; then
  bad "Termux sources.list contains Debian lines (clobbered): $sl"
  if [ "$FIX" = 1 ]; then
    cp "$sl" "$sl.dn-doctor.bak"
    printf 'deb https://packages-cf.termux.dev/apt/termux-main stable main\n' > "$sl"
    ok "restored Termux sources.list (backup $sl.dn-doctor.bak)"
  fi
elif [ -f "$sl" ]; then
  ok "Termux sources.list looks intact"
else
  warn "no Termux sources.list at $sl"
fi
[ -f "$TP/etc/apt/sources.list.d/glibc.list" ] \
  && ok "glibc.list present" \
  || warn "glibc.list missing (glibc side-install repo)"

# 4. termux-apt/termux-dpkg exist, and no 0.1.x routing wrapper is left on
#    PATH (it would shadow Termux's apt for pkg).
stale=""
for n in apt apt-get apt-cache dpkg; do
  [ -f "$LAUNCHDIR/$n" ] && grep -q "deb-native arch-aware" "$LAUNCHDIR/$n" && stale="$stale $n"
done
if [ -x "$LAUNCHDIR/termux-apt" ] && [ -x "$LAUNCHDIR/termux-dpkg" ] && [ -z "$stale" ]; then
  ok "termux-apt/termux-dpkg present, no old routing wrappers"
else
  [ -n "$stale" ] && warn "0.1.x routing wrappers still on PATH:$stale"
  [ -x "$LAUNCHDIR/termux-apt" ] || warn "no termux-apt/termux-dpkg (run make-apt-wrappers.sh)"
  if [ "$FIX" = 1 ] && [ -d "$ROOT" ]; then
    sh "$REPO/scripts/runtime/make-apt-wrappers.sh" "$ROOT" && ok "regenerated wrappers"
  fi
fi

# 5. Prefix state.
if [ -s "$DN_PREFIX/var/lib/dpkg/status" ]; then
  n=$([ -f "$DN_PREFIX/var/lib/dpkg/status" ] && grep -c "install ok installed" "$DN_PREFIX/var/lib/dpkg/status" || true)
  ok "prefix dpkg db has ${n:-0} installed packages"
else
  warn "no dpkg db at $DN_PREFIX (not bootstrapped yet?)"
fi

if [ "$FIX" = 1 ] && [ -d "$LAUNCHDIR" ]; then
  sh "$REPO/scripts/runtime/make-shell-interface.sh" "$ROOT" && ok "reinstalled the shell interface (start a new session)"
fi

[ "$fail" = 0 ] && echo "==> ok" || echo "==> problems found (re-run with --fix to repair)"
exit "$fail"
