#!/bin/sh
# Rewrite absolute symlinks inside a deb-native prefix so the kernel resolves
# them within the prefix.  Under bind-only tracing (tracer/, docs/spec/tracer/bind-only.md)
# the guest path is prefix-rewritten and handed to the kernel: a symlink whose
# target is a guest-absolute path under a bound dir (/usr /etc /var /opt /bin
# /sbin) would otherwise be followed against the real host "/", not $INSTDIR.
# Convert such targets to relative paths computed inside the prefix.
#
# Idempotent; safe to run after every package install.
#
#   scripts/normalize-symlinks.sh PREFIX_ROOT      # e.g. ~/dn6/root
set -eu

ROOT=${1:?usage: normalize-symlinks.sh PREFIX_ROOT}
case "$ROOT" in /*) ;; *) ROOT="$PWD/$ROOT" ;; esac
BOUND="usr etc var opt bin sbin tmp run"

[ -d "$ROOT" ] || exit 0

# Print the path of TARGET relative to DIR, both absolute host paths.
relpath() {
    awk -v from="$1" -v to="$2" 'BEGIN {
        nf = split(from, a, "/");
        nt = split(to, b, "/");
        i = 1;
        while (i <= nf && i <= nt && a[i] == b[i]) i++;
        out = "";
        for (j = i; j <= nf; j++) out = out (out == "" ? "" : "/") "..";
        for (j = i; j <= nt; j++) out = out (out == "" ? "" : "/") b[j];
        if (out == "") out = ".";
        print out;
    }'
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

pass=0
changed=1
while [ "$changed" -eq 1 ] && [ "$pass" -lt 5 ]; do
    pass=$((pass + 1))
    changed=0

    find "$ROOT" -xdev -type l > "$tmp" || true
    while IFS= read -r link; do
        target=$(readlink "$link") || continue

        # Only absolute targets can escape the prefix.
        case "$target" in
        /*)
          # Only targets that land in a bound directory matter.
          rest=${target#/}
          first=${rest%%/*}
          case " $BOUND " in *" $first "*) ;; *) continue ;; esac

          host_target="$ROOT$target"
          rel=$(relpath "$(dirname "$link")" "$host_target")
          [ "$rel" = "$target" ] && continue

          ln -sfn "$rel" "$link"
          changed=1
          ;;
        *)
          # A RELATIVE target that does not resolve: a package (or
          # update-alternatives) may compute it against a logical dir that is
          # a merged-usr symlink in the prefix. e.g. /bin/nc ->
          # ../etc/alternatives/nc is right from /bin, but the prefix's
          # bin -> usr/bin, so the link physically lives in usr/bin and ../etc
          # resolves to usr/etc, which does not exist (netcat-openbsd). If the
          # logical interpretation lands on a real file, rewrite the link
          # relative to its physical directory.
          [ -e "$link" ] && continue
          case "$link" in "$ROOT/usr/bin/"*|"$ROOT/usr/sbin/"*) ;; *) continue ;; esac
          case "$target" in ../*) ;; *) continue ;; esac
          cand="$ROOT/${target#../}"
          [ -e "$cand" ] || continue
          rel=$(relpath "$(dirname "$link")" "$cand")
          ln -sfn "$rel" "$link"
          changed=1
          ;;
        esac
    done < "$tmp"
done

echo "Normalized symlinks in $ROOT ($pass pass(es))."
