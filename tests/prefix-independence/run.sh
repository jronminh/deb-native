#!/bin/sh
# R0 acceptance -- the prefix runtime is independent of Termux's tree
# (TODO.md 0.7.0, docs/spec/userlands.md). Runs the deployed prefix's own
# userland with DN_TERMUX_PREFIX pointed at an EMPTY dir, so any command that
# fell back to Termux's $PREFIX fails; every check must pass using only files
# under PREFIX. Does NOT bootstrap and does NOT download (unless
# DN_INDEP_APT=1). Point it at a prefix already deployed.
#
# Usage: run.sh PREFIX
# Env: DN_INDEP_APT=1  also install a small package with the prefix's apt
#                      (needs network), then run it.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
P=${1:?usage: run.sh PREFIX}
case "$P" in /*) ;; *) P="$PWD/$P" ;; esac
[ -x "$P/usr/bin/dn-shell" ] || { echo "not a deb-native prefix: $P" >&2; exit 1; }
tp=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
HOME_REAL=${HOME:-/data/data/com.termux/files/home}

fail() { printf 'FAIL: prefix-independence: %s\n' "$1" >&2; exit 1; }
ok()   { printf '  ok    %s\n' "$1"; }
note() { printf '  note  %s\n' "$1"; }

# A writable TMPDIR for the harness's own temp dirs: the caller's if usable,
# else Termux's default, else the prefix itself.
if [ ! -d "${TMPDIR:-/nonexistent}" ] || ! touch "${TMPDIR:-/nonexistent}/.t.$$" 2>/dev/null; then
  TMPDIR="$tp/tmp"; [ -d "$TMPDIR" ] || TMPDIR="$P"
fi
rm -f "$TMPDIR/.t.$$" 2>/dev/null || true
export TMPDIR

EMPTY=$(mktemp -d)
TMP=$(mktemp -d)
trap 'rm -rf "$EMPTY" "$TMP"' EXIT
in_ul() { TMPDIR="$TMP" DN_TERMUX_PREFIX="$EMPTY" "$P/usr/bin/dn-shell" -c "$1" 2>&1; }

# 1. shell + coreutils resolve inside the prefix
case "$(in_ul 'command -v bash')" in "$P"/*) ;; *) fail "bash is not the prefix's";; esac
in_ul 'ls -1 /usr/bin >/dev/null && sed --version >/dev/null && grep --version >/dev/null' \
  || fail "coreutils/sed/grep did not run"
ok "shell + coreutils resolve inside the prefix"

# 2. Debian's own dpkg/apt run
case "$(in_ul 'dpkg --version | head -1')" in *"Debian 'dpkg'"*) ;; *) fail "dpkg is not Debian's";; esac
case "$(in_ul 'apt-get --version | head -1')" in apt*) ;; *) fail "apt-get did not run";; esac
ok "Debian dpkg/apt run"

# 3. identity: getent answers from the prefix's own /etc
case "$(in_ul 'getent passwd root')" in root:*) ;; *) fail "getent passwd root failed";; esac
case "$(in_ul 'getent group root')" in root:*) ;; *) fail "getent group root failed";; esac
ok "identity: getent resolves users/groups from the prefix"

# 4. every redirect root is reachable through the overlay, with content only
#    the prefix has; /tmp and /run write into the prefix; /dev,/proc,/sys are
#    not pulled in. (readlink -f is unusable here: realpath(3) walks paths
#    internally, bypassing the shim.)
for p in /usr/bin/bash /etc/passwd /var/lib/dpkg/status /opt /bin/bash \
         /sbin/ldconfig /lib/aarch64-linux-gnu/libc.so.6 /root; do
  in_ul "test -e $p" || fail "overlay: $p not reachable in the prefix"
done
in_ul 'echo x > /tmp/.dnprobe' && [ -f "$P/tmp/.dnprobe" ] || fail "/tmp is not redirected into the prefix"
in_ul 'mkdir -p /run/.dnprobe && echo x > /run/.dnprobe/x' && [ -f "$P/run/.dnprobe/x" ] \
  || fail "/run is not redirected into the prefix"
in_ul 'rm -rf /tmp/.dnprobe /run/.dnprobe'
for d in dev proc sys; do [ ! -e "$P/$d" ] || fail "the prefix must not carry a /$d"; done
ok "overlay: /usr /etc /var /opt /bin /sbin /lib /tmp /run /root resolve in the prefix; /dev /proc /sys stay real"

# 5. no steady-state Termux path: PATH, and apt's own config
case "$(in_ul 'echo "$PATH"' | tr ':' '\n' | grep -c "^$tp/")" in
  0) ;; *) fail "PATH still lists a Termux dir";; esac
case "$(in_ul 'apt-config dump 2>/dev/null' | grep -c "$tp")" in
  0) ;; *) fail "apt config still references Termux";; esac
ok "no Termux dir in PATH or the prefix's apt config"

# 6. fake root: uid 0, chown a no-op
case "$(in_ul 'id -u')" in 0) ;; *) fail "not fake-root";; esac
in_ul 'touch /tmp/.dnfr && chown 1234:1234 /tmp/.dnfr && rm -f /tmp/.dnfr' \
  || fail "chown is not a no-op under fake root"
ok "fake root: uid=0 and chown is a no-op"

# 7. NSS files lookup (hosts) resolves in the prefix, no Termux
in_ul 'getent hosts localhost' >/dev/null 2>&1 || fail "getent hosts localhost failed"
ok "NSS resolves (hosts) from the prefix"

# 8. resolv.conf is the prefix's own, not a symlink into another tree
if [ -L "$P/etc/resolv.conf" ]; then
  tgt=$(readlink -f "$P/etc/resolv.conf" 2>/dev/null || true)
  case "$tgt" in "$P"/*) ;; *) fail "resolv.conf points outside the prefix: $tgt";; esac
fi
ok "resolv.conf is the prefix's own"

# 9. the runtime commands are present
for f in "$P/usr/bin/dn-shell" "$P/usr/lib/deb-native/dn-run" \
         "$P/usr/lib/deb-native/dn-shim.so" "$P/usr/lib/deb-native/bin/dn-adopt"; do
  [ -e "$f" ] || fail "missing runtime piece: $f"
done
[ -x "$P/usr/lib/deb-native/dn-trace" ] && ok "dn-trace present" \
  || note "no dn-trace (static/raw-syscall programs run untranslated)"
ok "runtime pieces present (dn-shell, dn-run, shim, dn-adopt)"

# 9b. Independence at the ELF level, not just in the environment. Masking
#     Termux with DN_TERMUX_PREFIX does NOT reach the Bionic host-layer
#     binaries' hard-coded Termux rpath (their clang link adds $tp/lib), and
#     dn-trace NEEDs libtalloc from there. Assert the rpath was retargeted
#     into the prefix and the libs were vendored, so a runtime never opens
#     $tp. (setup-runtime.sh does the vendoring + patchelf.)
HOST="$P/usr/lib/deb-native/host"
if in_ul 'command -v readelf >/dev/null 2>&1'; then
  for b in "$P/usr/lib/deb-native/dn-run" "$P/usr/lib/deb-native/dn-trace" \
           "$P/usr/bin/dn-shell" "$P/usr/bin/dn-perl"; do
    [ -e "$b" ] || continue
    rp=$(in_ul "readelf -d '$b' 2>/dev/null | grep -Ei '(RPATH|RUNPATH)'" || true)
    case "$rp" in
      *"$tp"*) fail "runtime ELF $b still points its rpath at $tp" ;;
    esac
  done
  ok "Bionic host ELFs carry no Termux rpath"
else
  note "no readelf in the prefix; skipping the static rpath check"
fi
if [ -x "$P/usr/lib/deb-native/dn-trace" ]; then
  [ -e "$HOST/libtalloc.so.2" ] \
    || fail "dn-trace is installed but libtalloc is not vendored in $HOST"
  ok "libtalloc is vendored in the prefix"
fi

# 9c. The Bionic preload the session inherits from Termux (termux-exec) is
#     remapped onto the prefix's own copy, so a Bionic child needs no $tp.
#     Launch through a Bionic parent (/system/bin/sh, the login's own shell,
#     no shim): a *shimmed* glibc parent rewrites LD_PRELOAD for its Bionic
#     children (dn-shim.c bionic_env), so it cannot hand dn-shell the
#     inherited preload directly.
if [ -e "$HOST/libtermux-exec-ld-preload.so" ] \
   && [ -e "$tp/lib/libtermux-exec-ld-preload.so" ] && [ -x /system/bin/sh ]; then
  got=$(/system/bin/sh -c 'LD_PRELOAD=$0 exec $1 -c "printf %s \"\$DN_BIONIC_PRELOAD\""' \
        "$tp/lib/libtermux-exec-ld-preload.so" "$P/usr/bin/dn-shell" 2>/dev/null || true)
  case "$got" in
    "$P"/*) ok "Bionic preload remapped into the prefix" ;;
    *)      fail "DN_BIONIC_PRELOAD still points outside the prefix: ${got:-<empty>}" ;;
  esac
else
  note "termux-exec not vendored (or absent in Termux); skipping remap check"
fi

# 9d. Exercise the tracer for real: dn-trace must load its libtalloc from the
#     vendored copy, with Termux's tree masked.
if [ -x "$P/usr/lib/deb-native/dn-run" ] && [ -x "$P/usr/lib/deb-native/dn-trace" ]; then
  in_ul "'$P/usr/lib/deb-native/dn-run' --trace /bin/true" \
    || fail "the tracer route did not run (dn-trace or libtalloc missing)"
  ok "tracer route runs with Termux's tree masked"
else
  note "no dn-run/dn-trace; skipping the tracer run"
fi

# 10. dn-adopt with no patchelf says so plainly (does not silently skip)
case "$(in_ul 'dn-adopt /bin/true' 2>&1)" in
  *"apt install patchelf"*) note "dn-adopt asks for patchelf (expected)" ;;
  *) ;; esac

# 11. optional end-to-end: the prefix's apt installs and runs a package
if [ "${DN_INDEP_APT:-0}" = 1 ]; then
  in_ul 'apt-get install -y hello >/dev/null 2>&1' >/dev/null 2>&1 \
    || in_ul 'apt-get install -y --reinstall hello >/dev/null 2>&1' >/dev/null 2>&1 \
    || fail "apt install in the prefix failed"
  case "$(in_ul 'hello')" in *"Hello, world!"*) ;; *) fail "installed program did not run";; esac
  ok "apt install + run with Termux's tree gone"
fi

echo "PASS: prefix-independence ($P)"
