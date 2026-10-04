#!/bin/sh
# Point gcc's own default dynamic linker at the prefix's glibc loader
# (docs/spec/dn-glibc-prefix.md; the earlier fix it replaces:
# docs/log/findings/gcc-hello-pt-interp-gap.md, 2026-10-01: "the shim's
# /lib gap, and a separate PT_INTERP wall").
#
# A binary gcc links itself (gcc -o prog prog.c) gets a literal, unresolvable
# PT_INTERP baked in: GCC's own aarch64-linux.h hardcodes
# "-dynamic-linker /lib/ld-linux-aarch64...so.1" into its *link spec --
# a real Debian path that doesn't exist on this Android device (no root, no
# chroot). The kernel resolves PT_INTERP itself at execve() time, before any
# userspace code (the path-redirect shim included) runs, so nothing at the
# libc-interposition layer can fix this; it has to be the string gcc's
# linker invocation writes into the ELF in the first place.
#
# Fix: GCC auto-loads an optional "specs" file next to its own libgcc.a if
# one is present, overriding its built-in defaults -- the site-local
# customization hook GCC ships for exactly this (same mechanism musl/Android
# NDK toolchains use), no patching or rebuilding gcc/binutils needed. This
# writes one with only the dynamic-linker string swapped for the fused
# loader's real, resolvable path -- everything else stays gcc's own default.
#
# Idempotent (skipped once a version's specs file already has the loader);
# a no-op if gcc isn't installed yet or the loader doesn't exist. Covers
# every installed gcc version under usr/lib/gcc/*/*/, not just the newest.
#
# Usage: dn-fix-gcc-specs.sh PREFIX
set -u
DN=${1:?usage: dn-fix-gcc-specs.sh PREFIX}
# Runtime interpreter: the prefix's own fused glibc loader (dn-glibc-prefix.md).
INTERP="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
[ -x "$INTERP" ] || exit 0
[ -d "$DN/usr/lib/gcc" ] || exit 0

# The exact literal GCC's aarch64-linux.h (GLIBC_DYNAMIC_LINKER) emits into
# every aarch64-linux-gnu gcc's *link spec, stable across the versions this
# project has seen (trixie's gcc-14); if a future gcc changes it, the sed
# below simply matches nothing and this becomes a silent no-op, caught by
# the usual end-to-end test (gcc -o hello hello.c && ./hello) rather than
# breaking the apt run.
OLD='/lib/ld-linux-aarch64%{mbig-endian:_be}%{mabi=ilp32:_ilp32}.so.1'

for d in "$DN"/usr/lib/gcc/*/*; do
  [ -d "$d" ] || continue
  target=$(basename "$(dirname "$d")")
  ver=$(basename "$d")
  specs="$d/specs"
  [ -f "$specs" ] && grep -q "$INTERP" "$specs" 2>/dev/null && continue
  gcc_bin="$DN/usr/bin/$target-gcc-$ver"
  [ -x "$gcc_bin" ] || gcc_bin="$DN/usr/bin/$target-gcc"
  [ -x "$gcc_bin" ] || continue
  "$gcc_bin" -dumpspecs 2>/dev/null | sed "s#$OLD#$INTERP#" > "$specs.new" \
    && mv "$specs.new" "$specs" \
    || rm -f "$specs.new"
done
exit 0
