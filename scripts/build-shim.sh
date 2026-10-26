#!/bin/sh
# Build dn-lane.so, the shim: a glibc DSO preloaded into every dynamic program a
# prefix runs.  It is a runtime artifact -- it knows nothing about which tree it
# will run in, only that the tree has a glibc -- but it is linked against a
# glibc, so this script takes that glibc from one of two places:
#
#   --in-prefix PREFIX  build inside PREFIX with its own gcc, through its boot.
#                       Zero ABI risk against that suite; the prefix needs a
#                       toolchain (a dev/full set has one).
#   --cross             build with aarch64-linux-gnu-gcc against its own sysroot.
#                       No prefix needed at all; the ABI floor is whatever glibc
#                       that toolchain carries.
#
# Usage: scripts/build-shim.sh --in-prefix PREFIX | --cross [OPTION...] [OUT]
#   OUT                 default: build/dn-lane.so
#   --host app|adb      the host profile (default app): lane.c has one
#                       -DDN_HOST_ADB site, so the two differ
#   --gen DIR           the directory holding policy-rows.inc (default:
#                       build/gen, generated here when it holds none)
#   --cancel            also patch glibc's shared cancellable svc
#                       (__syscall_cancel_arch) at the offset read from --libc;
#                       off by default, and about 1% slower for 204 fewer stops
#   --libc PATH         the libc whose offset is patched.  Required with
#                       --cancel, and it must be the libc the TARGET TREE will
#                       have -- patching a different build's offset corrupts its
#                       text.  The manifest records its sha256, so a mismatch is
#                       visible after the fact
#   --max-glibc X.Y     fail when the artifact needs a higher GLIBC_ symbol
#                       version than this.  The floor is set by the glibc the
#                       build links against, so this checks the builder, not the
#                       source
#
# Writes OUT and OUT.manifest (host, cc, cancel offset, the libc sha256).
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IN_PREFIX=
CROSS=0
HOST=app
GEN=
CANCEL=0
LIBC=
MAXGLIBC=
OUT=

while [ $# -gt 0 ]; do
  case $1 in
    --in-prefix) IN_PREFIX=$2; shift 2 ;;
    --cross) CROSS=1; shift ;;
    --host) HOST=$2; shift 2 ;;
    --gen) GEN=$2; shift 2 ;;
    --cancel) CANCEL=1; shift ;;
    --libc) LIBC=$2; shift 2 ;;
    --max-glibc) MAXGLIBC=$2; shift 2 ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "build-shim: unknown option $1" >&2; exit 2 ;;
    *) OUT=$1; shift ;;
  esac
done

die() { echo "build-shim: $*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

if [ -n "$IN_PREFIX" ] && [ "$CROSS" = 1 ]; then
  die "--in-prefix and --cross are two ways to find a glibc; give one"
fi
if [ -z "$IN_PREFIX" ] && [ "$CROSS" = 0 ]; then
  die "one of --in-prefix PREFIX or --cross is needed"
fi
case $HOST in app|adb) ;; *) die "--host is app or adb" ;; esac
[ -n "$OUT" ] || OUT=$SRC/build/dn-lane.so
GEN=${GEN:-$SRC/build/gen}

HOSTFLAG=
[ "$HOST" = adb ] && HOSTFLAG=-DDN_HOST_ADB

# the rows are the tracer's catalog; the shim compiles the same law against them
mkdir -p "$(dirname "$OUT")" "$GEN"
if [ ! -f "$GEN/policy-rows.inc" ]; then
  command -v python3 >/dev/null 2>&1 || die "python3 not found (it generates the rows)"
  python3 "$SRC/src/dn-init/gen_rows.py" "$GEN/policy-rows.inc"
fi
ROWS=$GEN/policy-rows.inc

CANCELFLAG=
if [ "$CANCEL" = 1 ]; then
  [ -n "$LIBC" ] || die "--cancel needs --libc PATH (the target tree's libc)"
  [ -f "$LIBC" ] || die "no such libc: $LIBC"
  command -v python3 >/dev/null 2>&1 || die "python3 not found (it reads the offset)"
  off=$(python3 "$SRC/scripts/find-cancellable.py" "$LIBC" 2>/dev/null) || off=
  [ -n "$off" ] || die "no cancellable svc found in $LIBC"
  off=${off##*offset }
  off=${off%% *}
  CANCELFLAG="-DDN_CANCEL_OFF=$off"
  say "the cancellable svc: $off (in $LIBC)"
fi

CCNAME=
if [ "$CROSS" = 1 ]; then
  CC=${CC:-aarch64-linux-gnu-gcc}
  command -v "$CC" >/dev/null 2>&1 || die "no $CC (apt-get install gcc-aarch64-linux-gnu)"
  CCNAME=$("$CC" -dumpmachine 2>/dev/null || echo "$CC")
  say "the shim: dn-lane.so, cross-compiled with $CCNAME"
  # shellcheck disable=SC2086
  "$CC" -O2 -Wall -Wextra -shared -fPIC -DDN_LIBC $CANCELFLAG $HOSTFLAG \
      -I"$SRC/src/dn-init" -I"$GEN" \
      -o "$OUT" "$SRC/src/dn-init/lane.c" "$SRC/src/dn-init/law.c"
else
  P=$(CDPATH= cd -- "$IN_PREFIX" && pwd) || die "no such prefix: $IN_PREFIX"
  [ -x "$P/boot/boot.sh" ] || die "$P has no boot/boot.sh"
  [ -x "$P/usr/bin/gcc" ] || die "$P has no gcc; a dev or full set does, or use --cross"
  T=$P/tmp/dn-lane
  say "the shim: dn-lane.so, built inside $P against its glibc"
  rm -rf "$T"
  mkdir -p "$T"
  cp "$SRC/src/dn-init/lane.c" "$SRC/src/dn-init/lane.h" \
     "$SRC/src/dn-init/law.c" "$SRC/src/dn-init/law.h" "$T/"
  cp "$ROWS" "$T/policy-rows.inc"
  # gcc runs inside the tree, so the paths it is given are the tree's
  # shellcheck disable=SC2086
  "$P/boot/boot.sh" gcc -O2 -Wall -Wextra -shared -fPIC -DDN_LIBC $CANCELFLAG $HOSTFLAG \
      -I/tmp/dn-lane -o /tmp/dn-lane/dn-lane.so \
      /tmp/dn-lane/lane.c /tmp/dn-lane/law.c
  cp "$T/dn-lane.so" "$OUT"
  rm -rf "$T"
  CCNAME=$("$P/boot/boot.sh" gcc -dumpversion 2>/dev/null || echo gcc)
fi

# The ABI floor: the highest glibc symbol version the artifact needs.  It is a
# property of the glibc it was linked against -- not a choice made in this file,
# and not something to work around in the source -- so it is recorded, and
# --max-glibc turns the record into a check.
READELF=
for r in readelf llvm-readelf aarch64-linux-gnu-readelf; do
  if command -v "$r" >/dev/null 2>&1; then READELF=$r; break; fi
done
floor=unknown
if [ -n "$READELF" ]; then
  floor=$("$READELF" -W --version-info "$OUT" 2>/dev/null |
          grep -o 'GLIBC_[0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -1) || floor=
  [ -n "$floor" ] || floor=unknown
fi
if [ -n "$MAXGLIBC" ] && [ "$floor" != unknown ]; then
  over=$(awk -v a="$floor" -v b="$MAXGLIBC" 'BEGIN {
           split(a, x, "."); split(b, y, ".");
           if (x[1] + 0 > y[1] + 0 || (x[1] + 0 == y[1] + 0 && x[2] + 0 > y[2] + 0))
             print 1
         }')
  [ -z "$over" ] || die "$OUT needs GLIBC_$floor, above --max-glibc $MAXGLIBC: link against an older glibc (an older cross toolchain, or an older prefix), which is a property of the builder, not of the source"
fi

# The glibc it was linked against, for the record: --libc if given, else the
# toolchain's sysroot (cross) or the prefix's own (in-prefix).
libc=$LIBC
if [ -z "$libc" ] && [ "$CROSS" = 1 ]; then
  # the exact file the link resolves against, not a guess from the sysroot: the
  # cross gcc reports "/" as its sysroot and keeps the target libc elsewhere
  libc=$("$CC" -print-file-name=libc.so.6 2>/dev/null) || libc=
  case $libc in ''|libc.so.6) libc= ;; esac
  [ -n "$libc" ] && [ ! -f "$libc" ] && libc=
fi
if [ -z "$libc" ] && [ -n "${P:-}" ]; then
  for l in "$P"/usr/lib/*/libc.so.6; do
    if [ -f "$l" ]; then libc=$l; break; fi
  done
fi
libc_sha=-
if [ -n "$libc" ]; then
  command -v readlink >/dev/null 2>&1 && libc=$(readlink -f "$libc" 2>/dev/null || printf '%s' "$libc")
fi
if [ -n "$libc" ] && command -v sha256sum >/dev/null 2>&1; then
  libc_sha=$(sha256sum "$libc" | cut -d' ' -f1)
fi

cat > "$OUT.manifest" <<M
# dn-lane.so: what it was built against (scripts/build-shim.sh)
host=$HOST
cc=$CCNAME
cancel=${off:-0}
glibc_floor=${floor}
libc=${libc:-none}
libc_sha256=$libc_sha
M

say "glibc floor: ${floor} (from ${libc:-unknown})"
echo "built $OUT ($(wc -c < "$OUT") bytes)"
