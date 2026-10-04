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

# A writable TMPDIR for the harness's own temp dirs: the caller's if usable,
# else Termux's default, else the prefix itself (the caller's may be stale,
# e.g. an inherited TMPDIR that no longer exists).
if [ ! -d "${TMPDIR:-/nonexistent}" ] || ! touch "${TMPDIR:-/nonexistent}/.t.$$" 2>/dev/null; then
  TMPDIR="$tp/tmp"; [ -d "$TMPDIR" ] || TMPDIR="$P"
fi
rm -f "$TMPDIR/.t.$$" 2>/dev/null || true
export TMPDIR

fail() { printf 'FAIL: prefix-independence: %s\n' "$1" >&2; exit 1; }
ok()   { printf '  ok    %s\n' "$1"; }

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
ok "identity: getent resolves root from the prefix"

# 4. the path overlay: absolute /usr and /etc resolve into the prefix
in_ul 'test -e /etc/passwd && ls -1 /usr/bin >/dev/null' || fail "path overlay broken"
ok "path overlay: /usr and /etc resolve in the prefix"

# 5. resolv.conf is the prefix's own, not a symlink into another tree
if [ -L "$P/etc/resolv.conf" ]; then
  tgt=$(readlink -f "$P/etc/resolv.conf" 2>/dev/null || true)
  case "$tgt" in "$P"/*) ;; *) fail "resolv.conf points outside the prefix: $tgt";; esac
fi
ok "resolv.conf is the prefix's own"

# 6. no steady-state Termux bin on PATH (informational until R1 lands)
if in_ul 'echo "$PATH"' | tr ':' '\n' | grep -qx "$tp/bin"; then
  printf '  note  PATH still lists %s/bin (R1 pending)\n' "$tp/bin"
else
  ok "PATH has no Termux bin dir"
fi

# 7. optional end-to-end: the prefix's apt installs a package, nothing of Termux
if [ "${DN_INDEP_APT:-0}" = 1 ]; then
  in_ul 'apt-get install -y --reinstall hello >/dev/null 2>&1 || apt-get install -y hello >/dev/null 2>&1' \
    || fail "apt install in the prefix failed"
  case "$(in_ul 'hello')" in *"Hello, world!"*) ;; *) fail "installed program did not run";; esac
  ok "apt install + run with Termux's tree gone"
fi

echo "PASS: prefix-independence ($P)"
