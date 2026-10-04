#!/bin/sh
# Create the maintainer-script runtime inside INSTDIR. Idempotent.
#
# Why this exists (docs/log/findings/complete-base-bootstrap.md):
# maintainer scripts are executed by the *kernel*
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
SRC="$HERE/../../native"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
BINDIR="$INSTDIR/usr/bin"
LIBDIR="$INSTDIR/usr/lib/deb-native"
# The privilege layer (docs/spec/design.md, TODO.md "sudo"): every command
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
# Replace TO atomically: write a sibling temp then rename(2). A prefix is
# live: the prefix's own glibc loader is the PT_INTERP of every Debian
# program and this script runs from inside that prefix, so a plain cp -f
# could leave a half-written interpreter for a process exec'ing during the
# copy.
put() {                                            # FROM TO
  if [ ! -e "$2" ] || ! cmp -s "$1" "$2"; then
    cp -f "$1" "$2.tmp.$$" && chmod 755 "$2.tmp.$$" && mv -f "$2.tmp.$$" "$2"
  fi
}

# The path-redirect shim (glibc LD_PRELOAD library).
if stale "$CACHE/path-redirect.so" "$SRC/path-redirect.c"; then
  "$HERE/../bootstrap/build-path-redirect.sh" "$CACHE/path-redirect.so"
fi
put "$CACHE/path-redirect.so" "$LIBDIR/path-redirect.so"

# Launch dispatcher: classifies a target's ELF PT_INTERP at launch and picks
# the shim (glibc), plain exec (Bionic), or the dn-trace syscall rewrite
# (static, which the shim cannot reach). Built with Termux's own clang -- it
# is a Bionic binary and must run before any glibc env is set up.
if stale "$CACHE/dn-run" "$SRC/dn-run.c"; then
  echo "Building dn-run ..."
  clang -O2 -o "$CACHE/dn-run" "$SRC/dn-run.c"
fi
put "$CACHE/dn-run" "$LIBDIR/dn-run"

# The syscall tracer (fork-lite, tracer/) for what the shim cannot
# reach: static binaries (which Android's seccomp filter also kills without
# its syscall emulation), programs making their own syscalls, NSS. Built here
# when its build needs are present (pkg install make libtalloc), once per
# checkout like the rest; without them those programs run untranslated
# (dn-run warns; there is no fallback to Termux's proot).
TRACER="$HERE/../../tracer"
if [ -x "$(command -v make || true)" ] && [ -e "$PREFIX_DIR/lib/libtalloc.so" ]; then
  if [ ! -x "$TRACER/dn-trace" ] || [ -n "$(find "$TRACER" -name '*.[ch]' -newer "$TRACER/dn-trace" | head -n1)" ]; then
    echo "Building the tracer (dn-trace) ..."
    # From clean: dependency files of a removed source break an
    # incremental build ("No rule to make target").
    make -s -C "$TRACER" clean
    make -C "$TRACER" CC=clang
  fi
else
  echo "W: tracer not built (needs: pkg install make libtalloc); static programs will run untranslated"
fi
if [ -x "$TRACER/dn-trace" ]; then
  put "$TRACER/dn-trace" "$LIBDIR/dn-trace"
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
# path the shim points it at
# (docs/log/findings/proper-base-bootstrap.md, next step 1); dpkg-statoverride
# isn't shipped by Termux's own dpkg either (TODO.md quick wins), and
# without a shim its postinst call prints a bare "command not found" on
# every install (e.g. ca-certificates) even though it still reaches ii.
# These are fine as scripts -- they are reached through PATH (a normal
# exec, not a shebang chain), so the one-level rule does not apply.
# update-rc.d/invoke-rc.d (deb-systemd-helper/-invoke for a systemd unit)
# register and start a package's service: the prefix has no init system
# and no boot, so a package that merely ships one (nethack-common's save
# recovery, survey 2026-09-27) installs, and the service is not run. Running services is the planned runit translation
# (TODO.md), not this.
# ldconfig: libc-bin's own postinst/trigger calls `ldconfig -r "$DPKG_ROOT/"`
# unconditionally (Debian's chroot-style cache rebuild) -- root-only like
# chown/chgrp above, chroot(2) always fails unprivileged. The real,
# SYSCONFDIR-targeted rebuild (no -r) is dn-install-glibc.sh's own job, run
# by full path, never through this name on PATH.
for n in chown chgrp dpkg-statoverride update-rc.d invoke-rc.d deb-systemd-helper deb-systemd-invoke ldconfig; do
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
# prefixes). Its links are absolute, which the
# kernel follows against Android's root, so they are made relative at once
# (dn-fix-alternatives.sh): until then the command itself (awk) is broken.
cat > "$PRIV/update-alternatives" <<EOF
#!/system/bin/sh
DPKG_ROOT="$INSTDIR" "$INSTDIR/usr/bin/update-alternatives" --altdir "$INSTDIR/etc/alternatives" --admindir "$INSTDIR/var/lib/dpkg/alternatives" --log /var/log/alternatives.log "\$@"
rc=\$?
"$HERE/dn-fix-alternatives.sh" "$INSTDIR"
exit \$rc
EOF
chmod 755 "$PRIV/update-alternatives"
rm -f "$BINDIR/update-alternatives"   # 0.1.x location
# dpkg-divert has the same bug: under DPKG_ROOT it joins DPKG_ROOT with its
# compiled-in admindir (traced: $ROOT$PREFIX/var/lib/dpkg/diversions).
cat > "$PRIV/dpkg-divert" <<EOF
#!/system/bin/sh
exec "$INSTDIR/usr/bin/dpkg-divert" --admindir "$INSTDIR/var/lib/dpkg" --instdir "$INSTDIR" "\$@"
EOF
chmod 755 "$PRIV/dpkg-divert"

# chroot -- TEMPORARY FIX (hotfix 0.2.1). dpkg sets DPKG_ROOT to the prefix
# for maintainer scripts, and Debian's DPKG_ROOT support runs commands as
# `chroot "$DPKG_ROOT" CMD` (dbus-system-bus-common's postinst). Android's
# seccomp kills chroot(2) with SIGSYS (exit 159), failing the package and
# every later apt run. A chroot into the prefix (or /) is what the shim
# already provides, so the command runs directly, with an absolute program
# path taken from the prefix; --userspec/--groups are ignored (one user).
# Any other root is refused with a message. To be replaced by the planned
# identity/services layer (TODO.md, "After alpha"), which also handles the
# system users such scripts go on to create.
cat > "$PRIV/chroot" <<EOF
#!/system/bin/sh
while [ \$# -gt 0 ]; do
  case "\$1" in
    --userspec=*|--groups=*|--skip-chdir) shift ;;
    --userspec|--groups) shift 2 ;;
    --) shift; break ;;
    -*) echo "chroot (deb-native): option \$1 not supported" >&2; exit 125 ;;
    *) break ;;
  esac
done
root=\${1:?chroot (deb-native): missing NEWROOT}; shift
real=\$(cd "\$root" 2>&1 && pwd -P) || { echo "chroot (deb-native): \$root: no such directory" >&2; exit 125; }
dn=\$(cd "$INSTDIR" && pwd -P)
if [ "\$real" != "\$dn" ] && [ "\$real" != / ]; then
  echo "chroot (deb-native): chroot to \$root is not supported (Android forbids chroot; only the prefix itself)" >&2
  exit 125
fi
cd "$INSTDIR" || exit 125
[ \$# -gt 0 ] || exec "$INSTDIR/usr/bin/dn-shell" -i
case "\$1" in
  /*) [ -e "$INSTDIR\$1" ] && { p="$INSTDIR\$1"; shift; set -- "\$p" "\$@"; } ;;
esac
exec "\$@"
EOF
chmod 755 "$PRIV/chroot"

# getent: a maintainer script's account checks (passwd's postinst:
# `getent group shadow`, then groupadd if missing) reach Termux's glibc
# getent, whose NSS reads $PREFIX_DIR/glibc/etc -- libc-internal, so the
# shim cannot redirect it (survey 2026-09-27: passwd failed to configure,
# groupadd aborting on the audit interface). The account databases are
# answered from the prefix's own files; everything else (hosts, services,
# ...) goes to the real getent.
cat > "$PRIV/getent" <<EOF
#!/system/bin/sh
case "\$1" in
  passwd|group|shadow|gshadow) ;;
  *) exec "$INSTDIR/usr/bin/getent" "\$@" ;;
esac
db=\$1; shift
f="$INSTDIR/etc/\$db"
[ -r "\$f" ] || exit 2
if [ \$# -eq 0 ]; then
  while IFS= read -r line; do case "\$line" in ''|'#'*) ;; *) echo "\$line" ;; esac; done < "\$f"
  exit 0
fi
rc=0
for key in "\$@"; do
  hit=
  while IFS= read -r line; do
    name=\${line%%:*}; rest=\${line#*:}; rest=\${rest#*:}; id=\${rest%%:*}
    case "\$db" in passwd|group) ;; *) id= ;; esac
    if [ "\$key" = "\$name" ] || { [ -n "\$id" ] && [ "\$key" = "\$id" ]; }; then
      echo "\$line"; hit=1; break
    fi
  done < "\$f"
  [ -n "\$hit" ] || rc=2
done
exit \$rc
EOF
chmod 755 "$PRIV/getent"
