#!/bin/sh
# Create the maintainer-script runtime inside INSTDIR. Idempotent.
#
# Why this exists (docs/findings.md, and the
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
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../native"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
BINDIR="$INSTDIR/usr/bin"
LIBDIR="$INSTDIR/usr/lib/deb-native"
# The privilege layer (docs/design-0.2.0.md, TODO.md "sudo"): every command
# an unprivileged prefix has to fake or redirect lives here, first on a
# maintainer script's PATH (dn-launch.c), so a Debian package's real
# chown/update-alternatives never shadows it, and a later sudo/fake-root
# mode can replace this one directory.
PRIV="$LIBDIR/priv"

[ -x "$GLIBC/bin/bash" ] || { echo "E: no glibc bash at $GLIBC/bin/bash (pkg install bash-glibc)" >&2; exit 1; }

mkdir -p "$BINDIR" "$LIBDIR" "$PRIV"

# Build once per checkout, copy into each prefix: the shim, dn-run and
# dn-shell are compiled into native/.build/ (not in git) only when that copy
# is missing or its source is newer -- a fresh prefix, or a second one, then
# costs a copy instead of three clang runs. The prefix keeps its own copies
# so its launchers and wrappers have stable, self-contained paths.
CACHE="$SRC/.build"
mkdir -p "$CACHE"
stale() { [ ! -e "$1" ] || [ "$2" -nt "$1" ]; }   # ARTIFACT SOURCE
put() {                                            # FROM TO
  if [ ! -e "$2" ] || ! cmp -s "$1" "$2"; then cp -f "$1" "$2"; chmod 755 "$2"; fi
}

# The path-redirect shim (glibc LD_PRELOAD library).
if stale "$CACHE/path-redirect.so" "$SRC/path-redirect.c"; then
  "$HERE/build-path-redirect.sh" "$CACHE/path-redirect.so"
fi
put "$CACHE/path-redirect.so" "$LIBDIR/path-redirect.so"

# Launch dispatcher: classifies a target's ELF PT_INTERP at launch and picks
# the shim (glibc), plain exec (Bionic), or `proot -b` syscall rewrite
# (static, which the shim cannot reach). Built with Termux's own clang -- it
# is a Bionic binary and must run before any glibc env is set up.
if stale "$CACHE/dn-run" "$SRC/dn-run.c"; then
  echo "Building dn-run ..."
  clang -O2 -o "$CACHE/dn-run" "$SRC/dn-run.c"
fi
put "$CACHE/dn-run" "$LIBDIR/dn-run"

# The loader stub every Debian program names as its interpreter
# (dn-translate-deb.sh): sets up the shim, then hands over to glibc's real
# loader. Freestanding static PIE -- no libc, no relocations.
if stale "$CACHE/ld-dn" "$SRC/ld-dn.c"; then
  echo "Building ld-dn ..."
  clang -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
        -fPIE -Wl,-pie -Wl,--no-dynamic-linker -o "$CACHE/ld-dn" "$SRC/ld-dn.c"
fi
put "$CACHE/ld-dn" "$LIBDIR/ld-dn"

# The syscall tracer (fork-lite) used for static binaries and glibc NSS. Built
# from tracer/ (`make CC=clang`, needs libtalloc); install a prebuilt one if
# present, otherwise dn-run falls back to Termux's proot.
TRACER_SRC="$HERE/../tracer/proot"
if [ -x "$TRACER_SRC" ]; then
  put "$TRACER_SRC" "$LIBDIR/dn-trace"
fi

# The maintainer-script launcher. One binary, dispatched by its own argv[0]
# basename (dn-shell, dn-perl).
if stale "$CACHE/dn-shell" "$SRC/dn-launch.c"; then
  echo "Building dn-shell ..."
  clang -O2 -o "$CACHE/dn-shell" "$SRC/dn-launch.c"
fi
put "$CACHE/dn-shell" "$BINDIR/dn-shell"
put "$CACHE/dn-shell" "$BINDIR/dn-perl"

# No-op shims for root-only/unshipped commands a maintainer script may call
# by bare name: an unprivileged process cannot chown/chgrp no matter what
# path the shim points it at (findings.md, next step 1); dpkg-statoverride
# isn't shipped by Termux's own dpkg either (TODO.md quick wins), and
# without a shim its postinst call prints a bare "command not found" on
# every install (e.g. ca-certificates) even though it still reaches ii.
# These are fine as scripts -- they are reached through PATH (a normal
# exec, not a shebang chain), so the one-level rule does not apply.
for n in chown chgrp dpkg-statoverride; do
  printf '#!/system/bin/sh\nexit 0\n' > "$PRIV/$n"
  chmod 755 "$PRIV/$n"
  # 0.1.x put them in usr/bin, where a Debian package's real one would land.
  if [ -f "$BINDIR/$n" ] && head -c 40 "$BINDIR/$n" | grep -q '^#!/system/bin/sh'; then rm -f "$BINDIR/$n"; fi
done

# update-alternatives defaults its --altdir/--admindir to Termux's own real,
# compiled-in absolute path ($PREFIX_DIR/etc/alternatives, .../var/lib/dpkg/
# alternatives) -- an absolute path that does NOT start with a bare /usr,
# /etc, /var or /opt, so the shim correctly leaves it alone (it isn't a path
# this project should ever redirect). A postinst calling bare
# update-alternatives therefore writes a symlink into TERMUX's real
# alternatives directory instead of the prefix's own (found via a genuinely
# dangling `figlet -> $PREFIX_DIR/etc/alternatives/figlet` symlink). Force
# the prefix's own directories with a wrapper, same idea as the no-op shims
# above.
# --log too, root-relative with DPKG_ROOT set: update-alternatives joins
# DPKG_ROOT onto its log path even when given explicitly (found as
# $INSTDIR/data/data/com.termux/files/usr/var/log/alternatives.log, in 0.1.x
# prefixes and on the naibed branch). Its links are absolute, which the
# kernel follows against Android's root, so they are made relative at once
# (dn-fix-alternatives.sh): until then the command itself (awk) is broken.
cat > "$PRIV/update-alternatives" <<EOF
#!/system/bin/sh
DPKG_ROOT="$INSTDIR" "$PREFIX_DIR/bin/update-alternatives" --altdir "$INSTDIR/etc/alternatives" --admindir "$INSTDIR/var/lib/dpkg/alternatives" --log /var/log/alternatives.log "\$@"
rc=\$?
"$HERE/dn-fix-alternatives.sh" "$INSTDIR"
exit \$rc
EOF
chmod 755 "$PRIV/update-alternatives"
rm -f "$BINDIR/update-alternatives"   # 0.1.x location
# dpkg-divert has the same bug: under DPKG_ROOT it joins DPKG_ROOT with its
# compiled-in admindir (traced on naibed: $ROOT$PREFIX/var/lib/dpkg/diversions).
cat > "$PRIV/dpkg-divert" <<EOF
#!/system/bin/sh
exec "$PREFIX_DIR/bin/dpkg-divert" --admindir "$INSTDIR/var/lib/dpkg" --instdir "$INSTDIR" "\$@"
EOF
chmod 755 "$PRIV/dpkg-divert"
