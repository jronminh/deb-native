#!/bin/sh
# Install the pre-built runtime artifacts into a prefix, and create the
# privilege layer (the `priv/` stand-ins). Core: no building (build-core.sh).
# See MODULARIZE.md P2.
#
# Usage: install-runtime.sh INSTDIR
set -eu
INSTDIR=${1:?usage: install-runtime.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/core/native"
CACHE="$SRC/.build"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}
BINDIR="$INSTDIR/usr/bin"
LIBDIR="$INSTDIR/usr/lib/deb-native"
# The privilege layer (docs/spec/design.md, TODO.md "sudo"): every command
# an unprivileged prefix has to fake or redirect lives here, first on a
# maintainer script's PATH (dn-child.h), so a Debian package's real
# chown/update-alternatives never shadows it, and a later sudo/fake-root
# mode can replace this one directory.
PRIV="$LIBDIR/priv"

mkdir -p "$BINDIR" "$LIBDIR" "$PRIV"

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

# Copy the built artifacts; skip any that build-core.sh did not produce
# (the tracer is optional).
for a in dn-shim.so dn-run dn-trace; do
  [ -e "$CACHE/$a" ] || continue
  put "$CACHE/$a" "$LIBDIR/$a"
done

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
ua="$INSTDIR/usr/bin/update-alternatives"
[ -x "\$ua" ] || ua="$PREFIX_DIR/bin/update-alternatives"
DPKG_ROOT="$INSTDIR" "\$ua" --altdir "$INSTDIR/etc/alternatives" --admindir "$INSTDIR/var/lib/dpkg/alternatives" --log /var/log/alternatives.log "\$@"
rc=\$?
exit \$rc
EOF
chmod 755 "$PRIV/update-alternatives"
rm -f "$BINDIR/update-alternatives"   # 0.1.x location
# dpkg-divert has the same bug: under DPKG_ROOT it joins DPKG_ROOT with its
# compiled-in admindir (traced: $ROOT$PREFIX/var/lib/dpkg/diversions).
cat > "$PRIV/dpkg-divert" <<EOF
#!/system/bin/sh
d="$INSTDIR/usr/bin/dpkg-divert"
[ -x "\$d" ] || d="$INSTDIR/usr/sbin/dpkg-divert"
[ -x "\$d" ] || d="$PREFIX_DIR/bin/dpkg-divert"
exec "\$d" --admindir "$INSTDIR/var/lib/dpkg" --instdir "$INSTDIR" "\$@"
EOF
chmod 755 "$PRIV/dpkg-divert"

# dpkg-trigger: libc6's postinst calls it before any Debian dpkg exists in the
# prefix, and the prefix has no trigger system (services are out of scope), so
# a missing prefix dpkg-trigger is a no-op at bootstrap; once Debian's dpkg is
# installed (dn-install-aptdpkg.sh), the prefix's own runs. Never Termux's --
# that would register the trigger in Termux's own database.
cat > "$PRIV/dpkg-trigger" <<EOF
#!/system/bin/sh
t="$INSTDIR/usr/bin/dpkg-trigger"
[ -x "\$t" ] && exec "\$t" "\$@"
exit 0
EOF
chmod 755 "$PRIV/dpkg-trigger"

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
[ \$# -gt 0 ] || exec "$INSTDIR/usr/bin/bash" -i
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
  *) g="$INSTDIR/usr/bin/getent"; [ -x "\$g" ] || g="$GLIBC/bin/getent"; exec "\$g" "\$@" ;;
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
