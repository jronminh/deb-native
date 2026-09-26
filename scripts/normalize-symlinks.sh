#!/bin/sh
# Rewrite absolute symlinks inside a deb-native prefix so the kernel resolves
# them within the prefix.  Under bind-only tracing (tracer/, docs/bind-only.md)
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
BOUND="usr etc var opt bin sbin"

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

    find "$ROOT" -xdev -type l > "$tmp" 2>/dev/null || true
    while IFS= read -r link; do
        target=$(readlink "$link" 2>/dev/null) || continue

        # Only absolute targets can escape the prefix.
        case "$target" in /*) ;; *) continue ;; esac

        # Only targets that land in a bound directory matter.
        rest=${target#/}
        first=${rest%%/*}
        case " $BOUND " in *" $first "*) ;; *) continue ;; esac

        host_target="$ROOT$target"
        rel=$(relpath "$(dirname "$link")" "$host_target")
        [ "$rel" = "$target" ] && continue

        ln -sfn "$rel" "$link"
        changed=1
    done < "$tmp"
done

echo "normalize-symlinks: $ROOT (passes: $pass)"
