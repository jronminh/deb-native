#!/bin/sh
# Build the runtime artifacts from source into a cache shared with the
# installer. No prefix is touched (build stage; MODULARIZE.md P2).
#
# Artifacts: dn-shim.so (the shim), dn-run (launcher/classifier),
# dn-trace (the syscall tracer, optional), adbwire (optional). Built once per
# checkout into $SRC/.build; install-runtime.sh copies them into a prefix. The
# maintainer-script interpreters (dn-sh, dn-perl) are built by install-runtime
# instead, since they need the prefix's own gcc and loader (fork model).
#
# Usage: build-core.sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
SRC="$ROOT/core/native"
TRACER="$ROOT/core/tracer"
ADBWIRE_SRC="$ROOT/third_party/adbwire"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
CACHE="$SRC/.build"
mkdir -p "$CACHE"
stale() { [ ! -e "$1" ] || [ "$2" -nt "$1" ]; }   # ARTIFACT SOURCE

# The dn-shim shim (glibc LD_PRELOAD library).
if stale "$CACHE/dn-shim.so" "$SRC/dn-shim.c"; then
  "$HERE/build-dn-shim.sh" "$CACHE/dn-shim.so"
fi

# Launch dispatcher: classifies a target's ELF PT_INTERP at launch and picks
# the shim (glibc), plain exec (Bionic), or the dn-trace syscall rewrite
# (static, which the shim cannot reach). Built with Termux's own clang -- it
# is a Bionic binary and must run before any glibc env is set up.
if stale "$CACHE/dn-run" "$SRC/dn-run.c"; then
  echo "Building dn-run ..."
  clang -O2 -o "$CACHE/dn-run" "$SRC/dn-run.c"
fi

# The maintainer-script interpreters (dn-sh, dn-perl) are built by
# install-runtime.sh with the prefix's own gcc, so their NEEDED/PT_INTERP match
# the prefix's glibc and loader exactly (Stage 1, MODULARIZE.md "bootstrap
# stages"). build-core cannot: it has no prefix/toolchain.

# The syscall tracer (fork-lite, tracer/) for what the shim cannot
# reach: static binaries (which Android's seccomp filter also kills without
# its syscall emulation), programs making their own syscalls, NSS. Built here
# when its build needs are present (pkg install make libtalloc), once per
# checkout like the rest; without them those programs run untranslated
# (dn-run warns; there is no fallback to Termux's proot).
#
# The tracer is optional: detect each need separately, tolerate a build
# failure (the prefix still works with the shim alone), and say plainly
# whether dn-trace made it into the cache.
have_make=$(command -v make 2>/dev/null || true)
have_talloc=$(ls "$PREFIX_DIR"/lib/libtalloc.so* 2>/dev/null | head -n1 || true)
if [ -n "$have_make" ] && [ -n "$have_talloc" ]; then
  if [ ! -x "$TRACER/dn-trace" ] || [ -n "$(find "$TRACER" -name '*.[ch]' -newer "$TRACER/dn-trace" | head -n1)" ]; then
    echo "Building the tracer (dn-trace) ..."
    # From clean: dependency files of a removed source break an
    # incremental build ("No rule to make target").
    make -s -C "$TRACER" clean
    make -C "$TRACER" CC=clang || echo "W: tracer build failed; static/raw-syscall programs will run untranslated"
  fi
else
  [ -n "$have_make" ] || echo "W: 'make' not found (pkg install make); tracer not built"
  [ -n "$have_talloc" ] || echo "W: no libtalloc (pkg install libtalloc); tracer not built"
fi
if [ -x "$TRACER/dn-trace" ]; then
  cp -f "$TRACER/dn-trace" "$CACHE/dn-trace"
  echo "dn-trace built: static/NSS syscall routing available."
else
  echo "W: no dn-trace built; only the shim route is available."
fi

# The maintainer-script interpreters (dn-sh, dn-perl) are built by
# install-runtime.sh, not here: they need the prefix's own gcc and loader.

# adbwire (third_party/adbwire): termux-adb-bridge's daemonless
# Wireless-Debugging ADB client, so `dn-adbwire` can run one command per
# connection at Android's `shell` UID. Built here with Termux's clang +
# OpenSSL. Optional: with no clang/OpenSSL it is simply absent.
have_ssl=$(ls "$PREFIX_DIR"/lib/libssl.so* 2>/dev/null | head -n1 || true)
if [ -e "$ADBWIRE_SRC/adbwire.c" ] && command -v clang >/dev/null 2>&1 && [ -n "$have_ssl" ]; then
  if [ ! -x "$CACHE/adbwire" ] || [ -n "$(find "$ADBWIRE_SRC" -name '*.[ch]' -newer "$CACHE/adbwire" | head -n1)" ]; then
    echo "Building adbwire ..."
    clang -O2 -Wall -o "$CACHE/adbwire" \
      "$ADBWIRE_SRC/adbwire.c" "$ADBWIRE_SRC/spake2.c" \
      "$ADBWIRE_SRC/ed25519/fe.c" "$ADBWIRE_SRC/ed25519/ge.c" \
      "$ADBWIRE_SRC/ed25519/sc.c" "$ADBWIRE_SRC/ed25519/sha512.c" \
      "$ADBWIRE_SRC/ed25519/keypair.c" "$ADBWIRE_SRC/ed25519/sign.c" \
      "$ADBWIRE_SRC/ed25519/verify.c" "$ADBWIRE_SRC/ed25519/key_exchange.c" \
      -I"$ADBWIRE_SRC/ed25519" -lssl -lcrypto \
      || { echo "W: adbwire build failed"; rm -f "$CACHE/adbwire"; }
  fi
fi
