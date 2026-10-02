#!/bin/sh
# On-device smoke test for native/ld-dn.c's config layer
# (docs/spec/ld-dn-config.md). Builds ld-dn + the path shim + a glibc probe,
# assembles a throwaway prefix whose loader and libraries are symlinks into
# Termux's glibc side-install, repoints the probe's PT_INTERP at the built
# ld-dn, and asserts the environment ld-dn resolves -- from compiled
# defaults, from the shipped default config, from explicit overrides, and
# with a malformed config (which must fail open, not die).
#
# Fully self-contained: no root, no namespace, and the live prefix is never
# touched.
set -eu

G=${DN_GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)

[ -x "$G/bin/true" ] || { echo "no glibc side-install at $G (set DN_GLIBC_ROOT)"; exit 2; }
[ -d "$G/lib" ] || { echo "no glibc libraries at $G/lib"; exit 2; }

T=$(mktemp -d "$HOME/.cache/dn-ldtest.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT

LIBD=$T/usr/lib/deb-native
mkdir -p "$LIBD" "$T/usr/bin" "$T/usr/lib/aarch64-linux-gnu" "$T/etc/deb-native"

# Loader + libraries: symlink into the real glibc side-install, so ld-dn's
# default LD_LIBRARY_PATH finds them.
for so in "$G"/lib/*.so*; do
  ln -sf "$so" "$T/usr/lib/$(basename "$so")"
  ln -sf "$so" "$T/usr/lib/aarch64-linux-gnu/$(basename "$so")"
done

# Build ld-dn and the shim the way setup-runtime.sh does.
clang -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
      -fPIE -Wl,-pie -Wl,--no-dynamic-linker -o "$LIBD/ld-dn" "$REPO/native/ld-dn.c"
sh "$REPO/scripts/bootstrap/build-path-redirect.sh" "$LIBD/path-redirect.so" >/dev/null 2>&1

# The probe: a glibc binary, interpreter repointed at the built ld-dn.
clang --target=aarch64-linux-gnu --sysroot=/ -O0 \
  -nostartfiles -nodefaultlibs \
  -I"$G/include" -L"$G/lib" \
  -Wl,-dynamic-linker,"$G/lib/ld-linux-aarch64.so.1" \
  -o "$T/usr/bin/probe" \
  "$G/lib/Scrt1.o" "$G/lib/crti.o" "$HERE/probe.c" "$G/lib/crtn.o" -lc
patchelf --set-interpreter "$LIBD/ld-dn" "$T/usr/bin/probe"

run() { "$T/usr/bin/probe" 2>&1; }
fail=0
want() {
  if printf '%s\n' "$2" | grep -qxF "$1"; then :; else
    echo "MISSING: $1"; echo "--- got ---"; printf '%s\n' "$2"; fail=1
  fi
}
absent() {
  if printf '%s\n' "$2" | grep -qxF "$1"; then echo "UNEXPECTED: $1"; fail=1; fi
}

# 1. No config file: the compiled defaults.
rm -f "$T/etc/deb-native/ld-dn.conf"
OUT=$(run)
want "LD_PRELOAD=$LIBD/path-redirect.so" "$OUT"
want "DN_INSTDIR=$T" "$OUT"
want "LD_LIBRARY_PATH=$T/usr/lib/aarch64-linux-gnu:$T/usr/lib" "$OUT"
want "COMPILER_PATH=$T/usr/bin" "$OUT"

# 2. The shipped default config reproduces exactly the same policy.
cp "$REPO/native/ld-dn.conf" "$T/etc/deb-native/ld-dn.conf"
OUT=$(run)
want "LD_LIBRARY_PATH=$T/usr/lib/aarch64-linux-gnu:$T/usr/lib" "$OUT"
want "COMPILER_PATH=$T/usr/bin" "$OUT"

# 3. Overrides: lib-add, env, shim-prefix, and a per-program block that
#    applies to this program and one that must not.
cat > "$T/etc/deb-native/ld-dn.conf" <<EOF
lib-add /opt
env LOCPATH=\$DN/usr/lib/locale
shim-prefix /usr /etc
[probe]
env ONLYPROBE=1
[other]
env MARKER=nope
EOF
OUT=$(run)
want "LD_LIBRARY_PATH=$T/usr/lib/aarch64-linux-gnu:$T/usr/lib:$T/opt" "$OUT"
want "LOCPATH=$T/usr/lib/locale" "$OUT"
want "DN_REDIRECT_PREFIXES=/usr:/etc" "$OUT"
want "ONLYPROBE=1" "$OUT"
absent "MARKER=nope" "$OUT"

# 4. A malformed config must fail open: program still runs, defaults stand.
cat > "$T/etc/deb-native/ld-dn.conf" <<EOF
nonsense directive here
env
loader
lib-add /opt
EOF
OUT=$(run) || { echo "probe died on a malformed config"; fail=1; }
want "DN_INSTDIR=$T" "$OUT"
want "LD_LIBRARY_PATH=$T/usr/lib/aarch64-linux-gnu:$T/usr/lib:$T/opt" "$OUT"

if [ "$fail" -eq 0 ]; then
  echo "PASS: ld-dn-config (defaults, default file, overrides, fail-open)"
else
  echo "FAIL"; exit 1
fi
