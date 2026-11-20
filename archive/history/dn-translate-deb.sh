#!/bin/sh
# Translate one Debian .deb for the prefix, in place, before dpkg sees it.
#
#   - Architecture "all" -> "arm64" (the same rule the index is
#     read with, so apt and dpkg agree);
#   - hard links become copies: Android refuses link(2) in app data, and
#     perl-base ships one (perl5.40.1 -> perl);
#   - custom/<package>.sh, if present, applies per-package fixes;
#   - every glibc ELF whose interpreter is a Debian loader gets this prefix's
#     loader (the kernel loads it directly; the loader finds the prefix's
#     libraries through its own path);
#   - maintainer scripts (DEBIAN/preinst, postinst, prerm, postrm) and program
#     scripts get their "#!" line pointed at the prefix's own interpreter.
#
# Failures are fatal: a binary left with the wrong interpreter would install
# and fail later, so the translation exits 1 and apt runs nothing.
#
# One unpack and one repack per package, uncompressed (-Znone): the result
# only lives in a temp folder until dpkg installs it.
#
# Usage: dn-translate-deb.sh DEB_FILE PREFIX
set -eu
umask 022
DEB=${1:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
DN=${2:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
case "$DEB" in /*) ;; *) DEB="$PWD/$DEB" ;; esac
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

# The prefix's own ELF editor (overlay: dn-elf). It writes PT_INTERP in place
# when the path fits, and otherwise grows the highest PT_LOAD to map a longer
# one -- so a translation needs no patchelf.
ELF="$DN/usr/lib/deb-native/dn-elf"
[ -x "$ELF" ] || { echo "E: no $ELF -- cannot translate $DEB" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/pkg"
dpkg-deb -e "$DEB" "$WORK/pkg/DEBIAN"

# Hard links become copies: extract without them, then copy from the target.
dpkg-deb --fsys-tarfile "$DEB" | tar -tvf - |
  sed -n 's/^h.* \(\.\/[^ ]*\) link to \(\.\/.*\)$/\1\t\2/p' > "$WORK/hardlinks"
cut -f1 "$WORK/hardlinks" > "$WORK/hardlinks.exclude"
dpkg-deb --fsys-tarfile "$DEB" |
  tar -xpf - -C "$WORK/pkg" --no-wildcards --exclude-from="$WORK/hardlinks.exclude"
while IFS="$(printf '\t')" read -r link target; do
  cp -p "$WORK/pkg/$target" "$WORK/pkg/$link"
done < "$WORK/hardlinks"

PKG=$(sed -n 's/^Package: //p' "$WORK/pkg/DEBIAN/control")
sed -i 's/^Architecture: all$/Architecture: arm64/' "$WORK/pkg/DEBIAN/control"

if [ -x "$HERE/../../core/custom/$PKG.sh" ]; then
  "$HERE/../../core/custom/$PKG.sh" "$WORK/pkg" "$DN" >/dev/null
fi

# rewrite_shebang FILE MODE. MODE "maint" rewrites maintainer scripts (only the
# shell interpreters); MODE "program" rewrites any absolute interpreter that
# lives under /usr, /bin or /sbin. A shebang's flag (e.g. "/bin/sh -e") is kept
# and carried on the new line.
#
# The rewrite is a path change only. A literal /etc, /usr, /var path inside a
# script is never edited: dpkg exports DPKG_ROOT and most maintainer scripts
# already resolve through it, so editing the text would double the prefix.
rewrite_shebang() {
  f=$1; mode=$2
  [ "$(head -c2 "$f")" = "#!" ] || return 0
  line=$(head -n1 "$f")
  interp=$(printf '%s' "$line" | sed -E 's/^#![[:space:]]*([^[:space:]]+).*/\1/')
  rest=$(printf '%s' "$line" | sed -E 's/^#![[:space:]]*[^[:space:]]+//')
  case $interp in
    /bin/sh|/usr/bin/sh|/bin/dash|/usr/bin/dash) new="$DN/usr/bin/dash" ;;
    /bin/bash|/usr/bin/bash) new="$DN/usr/bin/bash" ;;
    /usr/bin/perl|/bin/perl)
      # perl is the prefix's own. Its @INC names /usr paths that do not exist on
      # the device, so the prefix's module dirs are put first by a BEGIN line
      # after the shebang (a shebang cannot carry this: it is cut at ~256 bytes).
      new="$DN/usr/bin/perl"
      { printf '#!%s%s\n' "$new" "$rest"
        printf 'BEGIN { unshift @INC, "%s/usr/share/perl5", "%s/usr/lib/aarch64-linux-gnu/perl-base", "%s/usr/lib/aarch64-linux-gnu/perl/5.40", "%s/usr/lib/aarch64-linux-gnu/perl5/5.40" }\n' "$DN" "$DN" "$DN" "$DN"
        tail -n +2 "$f"; } > "$WORK/shebang.new"
      cat "$WORK/shebang.new" > "$f"
      return 0 ;;
    /usr/*|/bin/*|/sbin/*)
      [ "$mode" = program ] || return 0
      new="$DN$interp" ;;
    *) return 0 ;;
  esac
  { printf '#!%s%s\n' "$new" "$rest"; tail -n +2 "$f"; } > "$WORK/shebang.new"
  cat "$WORK/shebang.new" > "$f"
}

# Every file is handled in this shell (not in a pipeline subshell), so a
# failure stops the translation.
find "$WORK/pkg" -path "$WORK/pkg/DEBIAN" -prune -o -type f -print > "$WORK/files"
while IFS= read -r f; do
  [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  # Static binaries and libraries without PT_INTERP: get-interp fails, which is
  # the expected answer, not an error.
  interp=$("$ELF" get-interp "$f" 2>/dev/null) || interp=""
  case $interp in
    */ld-linux-aarch64.so.1)
      if [ "$interp" != "$LD" ]; then
        "$ELF" set-interp "$f" "$LD" || {
          echo "E: cannot set the interpreter of ${f#"$WORK/pkg"}" >&2; exit 1; }
      fi ;;
  esac
done < "$WORK/files"

# update-alternatives, inside each maintainer script that calls it: a shell
# function of the same name runs the prefix's own binary and then makes the
# links it just wrote relative (a relative link never needs the prefix to be
# the real root). The script itself is not otherwise changed; links sit two
# levels below the prefix root in every alternatives group (usr/bin, usr/sbin,
# etc/alternatives), so "../../" is always the right relative path.
ALT_BLOCK='# deb-native: alternatives links stay relative inside the prefix.
_dn_ua() {
  _dn_rc=0
  __DN__/usr/bin/update-alternatives --altdir /etc/alternatives \
    --admindir /var/lib/dpkg/alternatives --log /var/log/alternatives.log "$@" || _dn_rc=$?
  _dn_relink
  return $_dn_rc
}
_dn_relink() {
  for _dn_a in "__DN__"/var/lib/dpkg/alternatives/*; do
    [ -f "$_dn_a" ] || continue
    _dn_n=0
    while IFS= read -r _dn_line; do
      _dn_n=$((_dn_n + 1))
      [ "$_dn_n" -eq 1 ] && continue
      [ -z "$_dn_line" ] && break
      if [ "$_dn_n" -eq 2 ] || [ $((_dn_n % 2)) -eq 0 ]; then _dn_fix "$_dn_line"
      else _dn_fix "/etc/alternatives/$_dn_line"; fi
    done < "$_dn_a"
    _dn_fix "/etc/alternatives/${_dn_a##*/}"
  done
}
_dn_fix() {
  _dn_l="__DN__$1"
  [ -L "$_dn_l" ] || return 0
  _dn_t=$(readlink "$_dn_l")
  case $_dn_t in /usr/*|/etc/*|/var/*|/opt/*|/bin/*|/sbin/*|/lib/*) ;; *) return 0 ;; esac
  ln -sfn "../../${_dn_t#/}" "$_dn_l"
}
'
ALT_BLOCK=$(printf '%s' "$ALT_BLOCK" | sed "s|__DN__|$DN|g")

for s in preinst postinst prerm postrm; do
  f="$WORK/pkg/DEBIAN/$s"
  [ -f "$f" ] || continue
  rewrite_shebang "$f" maint
  if grep -q 'update-alternatives' "$f"; then
    # Calls become _dn_ua (a valid shell name: dash refuses a function named
    # update-alternatives), then the function is placed after the shebang.
    sed -E 's/(^|[^A-Za-z0-9_-])update-alternatives([^A-Za-z0-9_-]|$)/\1_dn_ua\2/g' "$f" > "$WORK/alt.body"
    { head -n1 "$WORK/alt.body"; printf '%s\n' "$ALT_BLOCK"; tail -n +2 "$WORK/alt.body"; } > "$WORK/alt.new"
    cat "$WORK/alt.new" > "$f"
  fi
done

for d in usr/bin usr/sbin usr/games usr/libexec bin sbin; do
  [ -d "$WORK/pkg/$d" ] || continue
  find "$WORK/pkg/$d" -type f > "$WORK/progs"
  while IFS= read -r f; do rewrite_shebang "$f" program; done < "$WORK/progs"
done

# Silent on success: the packaging message and any noise go to a file, shown only
# when the repack fails.
dpkg-deb -Znone -b "$WORK/pkg" "$WORK/out.deb" >"$WORK/pack.log" 2>&1 || {
  cat "$WORK/pack.log" >&2
  echo "E: repacking $PKG failed" >&2
  exit 1
}
mv -f "$WORK/out.deb" "$DEB"
