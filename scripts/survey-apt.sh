#!/bin/sh
# Same idea and classification as survey.sh, but installing through real
# apt (setup-apt-prefix.sh + apt-install.sh) instead of bare dpkg on a
# single .deb — the before/after comparison for
# docs/findings-survey-2026-09-25.md's #1 finding (no dependency
# installer was the dominant failure cause, not native-seed coverage).
#
# Usage: OUT=~/survey-apt scripts/survey-apt.sh LIST.tsv
# LIST.tsv: "section<TAB>package<TAB>anything" (third column unused —
# apt resolves the .deb itself; kept so the same sample file from
# sample-packages.py works for both survey scripts).
set -uo pipefail

LIST=${1:?usage: OUT=DIR $0 LIST.tsv}
: "${OUT:?OUT: a directory for results}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$OUT/logs"
OUT=$(cd "$OUT" && pwd)
RES=$OUT/results.tsv
[ -s "$RES" ] || printf 'section\tpackage\tinstall\tdetail\n' > "$RES"

install_failure() {
  local log=$1
  if grep -qi 'Unable to locate package\|has no installation candidate' "$log"; then
    echo "gone"
  elif grep -qi 'Could not resolve host\|Connection timed out\|Failed to fetch' "$log"; then
    echo "network"
  elif grep -qi 'held broken packages\|Unable to correct problems\|unmet dependencies' "$log"; then
    echo "unresolvable $(grep -m1 -iE 'not (going to be installed|installable)|is not satisfiable' "$log" | cut -c1-200)"
  elif grep -qE 'script subprocess (returned error|failed)' "$log"; then
    echo "script $(grep -m1 -B3 'script subprocess' "$log" | tr '\n' ' ' | cut -c1-300)"
  elif grep -q 'error processing archive\|error processing package' "$log"; then
    echo "unpack $(grep -m1 -A2 'error processing' "$log" | tr '\n' ' ' | cut -c1-300)"
  else
    echo "other $(tail -3 "$log" | tr '\n' ' ' | cut -c1-300)"
  fi
}

survey_one() {
  local section=$1 pkg=$2 log=$OUT/logs/$pkg.log pfx=$OUT/work/$pkg
  rm -rf "$pfx"
  : > "$log"
  "$HERE/setup-apt-prefix.sh" "$pfx" >>"$log" 2>&1
  "$HERE/apt-install.sh" "$pfx" "$pkg" >>"$log" 2>&1
  local inst=ok detail=""
  if ! dpkg-query --admindir="$pfx/var/lib/dpkg" -W -f '${db:Status-Abbrev}' "$pkg" 2>/dev/null | grep -q '^ii'; then
    detail=$(install_failure "$log")
    inst=${detail%% *}
    detail=${detail#* }
    [ "$inst" != "$detail" ] || detail=""
  fi
  detail=$(printf '%s' "$detail" | tr '\t\n' '  ')
  printf '%s\t%s\t%s\t%s\n' "$section" "$pkg" "$inst" "$detail" >> "$RES"
  printf '%-16s %-40s %-12s %s\n' "$section" "$pkg" "$inst" "$detail" | cut -c1-200
}

while IFS=$'\t' read -r section pkg _; do
  case $section in section|'') continue ;; esac
  if cut -f2 "$RES" | grep -qxF -- "$pkg"; then continue; fi
  survey_one "$section" "$pkg"
done < "$LIST"
