#!/bin/sh
# Build the runtime's root with a plain glibc toolchain (the repo layout):
# dn-monitor, one self-contained binary.  Needs gcc and make (talloc is
# vendored) -- an arm64 Debian userland: a deb-native prefix with the
# toolchain installed, or a Debian arm64 host such as CI.  No Bionic
# toolchain, no Termux.
#
# dn-monitor is the tree's root process (docs/spec/monitor.md): the mechanism
# (the catch/route filter, the ptrace core) with the policy applier, dn-trace,
# linked in.  The syscall catalog (src/syscalls.tsv) is embedded at build time,
# so it reads no file at run time.
#
# dn-monitor is built in a scratch copy of src/tracer, so its objects never mix
# with the in-tree build.
#
# Usage: build-trace.sh [OUTDIR]   (default: src/.build-runtime)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/src"
OUT=${1:-$SRC/.build-runtime}
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
find "$T" \( -name '*.o' -o -name '*.d' -o -name dn-monitor -o -name dn-catalog.c \) -type f -exec rm -f {} +
# Self-contained/static when a stock glibc's lib dir is given (the arm64
# `libc6-dev` payload, extracted from Debian's own .deb): dn-trace is the
# outermost process -- nothing traces it -- and a poor host must start it
# with a bare exec and no loader at all.  A *stock* glibc's own startup is
# survivable here: its only Android-denied syscall at startup is `rseq`,
# and `GLIBC_TUNABLES=glibc.pthread.rseq=0` (a supported glibc tunable) turns
# that registration off (docs/reference/android-platform.md, "Stock libc6
# re-tested").  Link with that build's own start files (crt1.o ...), not the
# build host's glibc's.
if [ -n "${DN_GLIBC_LIBC_DIR:-}" ]; then
  make -s -C "$T" CC="$CC" CATALOG="$SRC/syscalls.tsv" DNPOLICY="$SRC/dn-policy" DNEMU="$SRC/dn-emu" LDFLAGS="-static -B$DN_GLIBC_LIBC_DIR -L$DN_GLIBC_LIBC_DIR -Wl,-z,noexecstack"
else
  make -s -C "$T" CC="$CC" CATALOG="$SRC/syscalls.tsv" DNPOLICY="$SRC/dn-policy" DNEMU="$SRC/dn-emu"
fi
cp -f "$T/dn-monitor" "$OUT/dn-monitor"

echo "runtime (glibc) in $OUT:"
ls -l "$OUT"
