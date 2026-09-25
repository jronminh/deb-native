#!/bin/sh
# Make the installed prefix transparently usable from Termux's own tools:
#
#   - put the launcher dir (wrappers for installed programs) first on PATH;
#   - export APT_CONFIG so Termux's own `apt`/`apt-get` read the prefix-scoped
#     config (the real Debian arm64 repo, Dir::* into the prefix, and the
#     patch/integrate hooks) -- no wrapper command, no new name;
#   - drop a `dpkg` wrapper on PATH so a direct `dpkg -i` runs against the
#     prefix, with the same patch/integrate steps as the apt hooks.
#
# All of it lives in one managed block in ~/.bashrc (idempotent, rewritten if
# the prefix moves); remove the block + the two generated files to undo.
#
# Usage: dn-activate.sh INSTDIR
set -eu
INSTDIR=${1:?usage: dn-activate.sh INSTDIR}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/.." && pwd)
DNPREFIX=$(dirname "$INSTDIR")
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
TERMUX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
RC=${DN_BASHRC:-$HOME/.bashrc}
MARK="# deb-native launchers (managed)"

[ -d "$LAUNCHDIR" ] || { echo "dn-activate: no launchers at $LAUNCHDIR (run make-launchers.sh)" >&2; exit 1; }

# `dpkg` wrapper: patch any .deb being installed, run real dpkg with the
# prefix's instdir/admindir/flags, then integrate. Mirrors the apt hooks for
# direct dpkg use.
cat > "$LAUNCHDIR/dpkg" <<EOF
#!/system/bin/sh
DN="$DNPREFIX"
ROOT="$INSTDIR"
REPO="$REPO"
REAL="$TERMUX/bin/dpkg"
debs=""
for a in "\$@"; do case "\$a" in *.deb) debs="\$debs \$a" ;; esac; done
[ -z "\$debs" ] || "\$REPO/scripts/apt-hook-pre.sh" "\$DN" \$debs
"\$REAL" --instdir="\$ROOT" --admindir="\$DN/var/lib/dpkg" \\
    --force-not-root --force-script-chrootless --force-architecture "\$@"
rc=\$?
"\$REPO/scripts/apt-hook-post.sh" "\$DN" || true
exit \$rc
EOF
chmod 755 "$LAUNCHDIR/dpkg"

if grep -qF "$MARK" "$RC" 2>/dev/null; then
  sed -i "\|$MARK|d; \|/usr/lib/deb-native/bin|d; \|APT_CONFIG=.*etc/apt.conf|d" "$RC"
fi
{
  echo ""
  echo "$MARK"
  echo "export PATH=\"$LAUNCHDIR:\$PATH\""
  echo "export APT_CONFIG=\"$DNPREFIX/etc/apt.conf\""
} >> "$RC"
echo "==> activated in $RC — run: . $RC"
