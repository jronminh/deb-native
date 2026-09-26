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
#
# Two opt-in env vars, added for fusion-no-prefix (docs/findings.md, "Bug 4"):
#
#   NORMALIZE_FUSE_USR=1   ROOT has no real nested usr/ (fusion mode's own
#                          premise) -- strip a leading /usr the same way
#                          path-redirect.c's DN_FUSE_USR does before joining
#                          with ROOT, so the computed host_target matches
#                          where the file actually lives ($ROOT/bin/x, not
#                          $ROOT/usr/bin/x). Off by default (classic design's
#                          ROOT really does have a nested usr/).
#
#   NORMALIZE_SCAN_DIRS="etc/alternatives bin share/man/man6"
#                          Scan only these ROOT-relative dirs (each
#                          non-recursive) instead of all of ROOT. Fusion
#                          mode's ROOT is Termux's own live, shared $PREFIX
#                          -- a full recursive scan there touches every
#                          symlink on the system, not just the ones a
#                          just-installed package's postinst touched. Unset
#                          keeps the original full-tree recursive scan
#                          (classic design's ROOT is its own small sandbox,
#                          where that scan is cheap and the intended
#                          behavior).
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

# Where TARGET (a bound-dir-rooted absolute path) actually lives under ROOT.
host_target_for() {
    t=$1
    if [ "${NORMALIZE_FUSE_USR:-}" = "1" ]; then
        case "$t" in
            /usr) t="/" ;;
            /usr/*) t=${t#/usr} ;;
        esac
    fi
    printf '%s' "$ROOT$t"
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

pass=0
changed=1
while [ "$changed" -eq 1 ] && [ "$pass" -lt 5 ]; do
    pass=$((pass + 1))
    changed=0

    if [ -n "${NORMALIZE_SCAN_DIRS:-}" ]; then
        : > "$tmp"
        for d in $NORMALIZE_SCAN_DIRS; do
            [ -d "$ROOT/$d" ] || continue
            find "$ROOT/$d" -maxdepth 1 -type l >> "$tmp" 2>/dev/null || true
        done
    else
        find "$ROOT" -xdev -type l > "$tmp" 2>/dev/null || true
    fi

    while IFS= read -r link; do
        target=$(readlink "$link" 2>/dev/null) || continue

        # Only absolute targets can escape the prefix.
        case "$target" in /*) ;; *) continue ;; esac

        # Only targets that land in a bound directory matter.
        rest=${target#/}
        first=${rest%%/*}
        case " $BOUND " in *" $first "*) ;; *) continue ;; esac

        host_target=$(host_target_for "$target")
        rel=$(relpath "$(dirname "$link")" "$host_target")
        [ "$rel" = "$target" ] && continue

        ln -sfn "$rel" "$link"
        changed=1
    done < "$tmp"
done

echo "normalize-symlinks: $ROOT (passes: $pass)"
