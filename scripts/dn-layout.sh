# Sourced (not executed) by the runtime and pipeline scripts once INSTDIR is
# set: where deb-native's own runtime files live inside INSTDIR.
#
# Classic prefix (main): INSTDIR is a tree of our own, laid out like Debian,
# so runtime files sit under usr/ (dn-shell and the no-op chown/chgrp in
# usr/bin, the shim and dn-run in usr/lib/deb-native).
#
# True fusion (docs/true-fusion.md): INSTDIR is Termux's own prefix,
# and $PREFIX/usr is a symlink to "." -- so usr/bin IS Termux's real bin/.
# Writing runtime files there would replace Termux's own binaries: the no-op
# chown/chgrp would shadow coreutils', and the update-alternatives wrapper
# would overwrite the real binary it execs. Everything goes under
# lib/deb-native/ instead; commands that must shadow a real one on a
# maintainer script's PATH go in lib/deb-native/fusion-bin, which dn-launch
# puts first.
DN_TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
if [ "$(cd "$INSTDIR" 2>/dev/null && pwd -P)" = "$(cd "$DN_TP" 2>/dev/null && pwd -P)" ]; then
  DN_FUSION=1
  DN_LIBDIR="$INSTDIR/lib/deb-native"
  DN_RTBIN="$DN_LIBDIR/fusion-bin"
else
  DN_FUSION=0
  DN_LIBDIR="$INSTDIR/usr/lib/deb-native"
  DN_RTBIN="$INSTDIR/usr/bin"
fi
DN_LAUNCHDIR="$DN_LIBDIR/bin"
