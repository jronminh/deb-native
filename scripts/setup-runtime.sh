#!/bin/sh
# Create the maintainer-script runtime inside INSTDIR. Idempotent.
#
# Why this exists (docs/findings-bootstrap-base-2026-09-25.md, and the
# on-device re-test 2026-09-25 PM that produced docs/findings-runtime-and-
# base-2026-09-25.md): maintainer scripts are executed by the *kernel*
# resolving their shebang, and the kernel follows only one `#!` level. An
# interpreter that is itself a shell script (`dn-shell` used to be) leaves
# dpkg falling back to Bionic /bin/sh with no shim and no glibc PATH --
# the root cause of the "CANNOT LINK EXECUTABLE /bin/sh" wall.
#
# So the interpreter is a real ELF: native/dn-launch.c, a tiny Bionic
# executable that sets LD_PRELOAD (= the path-redirect shim), DN_INSTDIR,
# PATH (prefix first, then Termux's glibc coreutils, then Termux's Bionic
# bin) and DEBIAN_FRONTEND, then execs Termux's already-present glibc bash
# (or perl for `dn-perl`). Built with Termux's own clang -- no cross
# toolchain, and it exists before any Debian package is installed, which
# also removes the old "need a Debian dash to run maintainer scripts"
# chicken-and-egg.
#
# Usage: setup-runtime.sh INSTDIR
set -eu
INSTDIR=${1:?usage: setup-runtime.sh INSTDIR}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../native"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
BINDIR="$INSTDIR/usr/bin"
LIBDIR="$INSTDIR/usr/lib/deb-native"

[ -x "$GLIBC/bin/bash" ] || { echo "setup-runtime: no glibc bash at $GLIBC/bin/bash" >&2; exit 1; }

mkdir -p "$BINDIR" "$LIBDIR"

# The shim is a build artifact; keep a copy inside the prefix so the
# launcher (and any wrapper) has a stable, self-contained path.
if [ ! -f "$LIBDIR/path-redirect.so" ] || [ "$SRC/path-redirect.c" -nt "$LIBDIR/path-redirect.so" ]; then
  "$HERE/build-path-redirect.sh" "$LIBDIR/path-redirect.so"
fi

# Build the launcher. One binary, dispatched by its own argv[0] basename.
if [ ! -x "$BINDIR/dn-shell" ] || [ "$SRC/dn-launch.c" -nt "$BINDIR/dn-shell" ]; then
  clang -O2 -o "$BINDIR/dn-shell" "$SRC/dn-launch.c"
  chmod 755 "$BINDIR/dn-shell"
  cp -f "$BINDIR/dn-shell" "$BINDIR/dn-perl"
fi

# No-op shims for root-only commands a maintainer script may call by bare
# name: an unprivileged process cannot chown/chgrp no matter what path the
# shim points it at (findings-bootstrap-base-2026-09-25.md, next step 1).
# These are fine as scripts -- they are reached through PATH (a normal
# exec, not a shebang chain), so the one-level rule does not apply.
printf '#!/system/bin/sh\nexit 0\n' > "$BINDIR/chown"
printf '#!/system/bin/sh\nexit 0\n' > "$BINDIR/chgrp"
chmod 755 "$BINDIR/chown" "$BINDIR/chgrp"
