#!/bin/sh
# Cross-compile native/path-redirect.c into a glibc shared library, using
# Termux's own clang (Bionic-hosted) targeting Termux's glibc side-install
# — the same technique Termux's own termux-pacman/glibc-packages build
# uses, discovered by trial and error (see docs/design-manual-overlay.md
# "Toolchain gotchas" for why each of these flags exists):
#
# - --target=aarch64-linux-gnu: glibc target, not Android/Bionic.
# - --sysroot=/: Termux's glibc/lib/libc.so is a linker script with
#   already-fully-resolved absolute paths (GROUP(/data/data/.../libc.so.6
#   ...)); a real --sysroot=$GLIBC makes lld re-root those absolute paths
#   under the sysroot AGAIN (doubling them into a nonexistent path).
#   --sysroot=/ disables that re-rooting so the linker script's paths are
#   used as-is.
# - -nostartfiles -nodefaultlibs: Termux's glibc-runner packages ship the
#   runtime shared libraries (libgcc_s.so) but not the static crt objects
#   (crtbeginS.o/crtendS.o/libgcc.a) a normal -shared link wants. Not
#   needed for a plain C shared library with no C++ exception handling.
# - explicit -Wl,-dynamic-linker: so the resulting .so's own metadata (not
#   load-bearing for an LD_PRELOAD .so, but kept consistent) points at
#   Termux's glibc ld.so, not Bionic's.

set -eu
GLIBC=${DN_GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-$HERE/native/path-redirect.so}

clang --target=aarch64-linux-gnu --sysroot=/ -O2 -fPIC -shared \
      -nostartfiles -nodefaultlibs \
      -I"$GLIBC/include" -L"$GLIBC/lib" \
      -Wl,-dynamic-linker,"$GLIBC/lib/ld-linux-aarch64.so.1" \
      -o "$OUT" "$HERE/native/path-redirect.c" -lc -ldl

echo "built: $OUT"
