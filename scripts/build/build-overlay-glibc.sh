#!/bin/sh
# Build the runtime overlay with a plain glibc toolchain (the repo layout):
# dn-trace, one self-contained binary.  Needs gcc and make (talloc is
# vendored) -- an arm64 Debian userland: a deb-native prefix with the
# toolchain installed, or a Debian arm64 host such as CI.  No Bionic
# toolchain, no Termux.
#
# Runtime v1 (docs/spec/overlay.md): the overlay is dn-trace, the tree's root
# process; the syscall catalog (src/syscalls.tsv) is embedded in it at build
# time, so it reads no file at run time.  The old LD_PRELOAD
# shim (dn-shim.so), the adopt-on-first-run launcher (dn-run) and the ELF
# editor (dn-elf) are gone: dn-policy wired into dn-glibc replaces the shim,
# the exec gate replaces dn-run, and TREE/RT are fixed at build time so no
# interpreter is relocated.
#
# dn-trace is built in a scratch copy of src/tracer, so its objects never mix
# with the in-tree build.
#
# Usage: build-overlay-glibc.sh [OUTDIR]   (default: src/.build-glibc)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/src"
OUT=${1:-$SRC/.build-glibc}
CC=${CC:-gcc}
mkdir -p "$OUT"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

for t in "$CC" make; do
  command -v "$t" >/dev/null 2>&1 || { echo "E: $t not found (a glibc toolchain is required)" >&2; exit 1; }
done
# A Bionic or cross compiler here would silently produce the wrong kind of
# binary: insist on a glibc target.
case "$("$CC" -dumpmachine)" in
  *-linux-gnu) ;;
  *) echo "E: $CC targets $("$CC" -dumpmachine), not glibc" >&2; exit 1 ;;
esac

echo "Building dn-trace ..."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp -R "$ROOT/src/tracer/." "$T/"
find "$T" \( -name '*.o' -o -name '*.d' -o -name dn-trace -o -name dn-catalog.c \) -type f -exec rm -f {} +
# Self-contained/static when an Android-patched glibc's lib dir is given: a
# poor host must start dn-trace with a bare exec, and a stock glibc is killed
# by Android's seccomp at startup, so link the patched libc.a in
# (docs/reference/android-platform.md, "Gate A").
if [ -n "${DN_GLIBC_LIBC_DIR:-}" ]; then
  make -s -C "$T" CC="$CC" CATALOG="$SRC/syscalls.tsv" DNPOLICY="$SRC/dn-policy" LDFLAGS="-static -L$DN_GLIBC_LIBC_DIR -Wl,-z,noexecstack"
else
  make -s -C "$T" CC="$CC" CATALOG="$SRC/syscalls.tsv" DNPOLICY="$SRC/dn-policy"
fi
cp -f "$T/dn-trace" "$OUT/dn-trace"

echo "overlay (glibc) in $OUT:"
ls -l "$OUT"
