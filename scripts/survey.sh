#!/bin/sh
# survey.sh — install a random sample of real Debian .deb packages, each
# into a fresh isolated prefix, and record where each one fails: while
# downloading, unpacking, configuring, or running. Adapted from
# sudo-less's dev/survey.sh (same classification logic, same idea: one
# fresh prefix per package so a broken one can't wedge the next) for
# deb-native's pipeline (scripts/prototype-install.sh + native-seed.sh
# instead of apt-get install — deb-native has no dependency resolver of
# its own yet, so a missing dependency is a distinct, expected failure
# category here, not "version skew" as in sudo-less's report).
#
# Usage:
#   MIRROR=https://deb.debian.org/debian OUT=~/survey scripts/survey.sh LIST.tsv
#
# LIST.tsv: "section<TAB>package<TAB>pool/path/to/file.deb" lines (a
# header line starting with "section" is skipped). Generate one with
# scripts/sample-packages.py against a downloaded Packages index.
#
# OUT/results.tsv gets one line per package:
#   section package install detail run programs
# install: ok, gone (404), network, unpack, depmissing (dpkg configure
#   failed — dependency native-seed.sh doesn't cover, deb-native's
#   closest equivalent to sudo-less's "skew"), script (maintainer script
#   failed), other
# run: none (no executable shipped), ok, partial, fail, untested (needs a
#   display/terminal, or hangs)
# programs: NAME:RESULT,... RESULT is ok, fail, gui, tty, hang
# OUT/logs/PKG.log keeps the dpkg output and each program's output.
set -uo pipefail

LIST=${1:?usage: OUT=DIR $0 LIST.tsv}
: "${OUT:?OUT: a directory for results}"
MIRROR=${MIRROR:-https://deb.debian.org/debian}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$OUT/logs" "$OUT/archives"
OUT=$(cd "$OUT" && pwd)
RES=$OUT/results.tsv
[ -s "$RES" ] || printf 'section\tpackage\tinstall\tdetail\trun\tprograms\n' > "$RES"
MAXPROG=${MAXPROG:-6}

# Why the install failed, from the log in $1.
install_failure() {
  local log=$1
  if grep -q '404 Not Found\|curl:.*(22)' "$log"; then
    echo "gone"
  elif grep -qi 'Could not resolve host\|Connection timed out\|Failed to connect' "$log"; then
    echo "network"
  elif grep -qE 'dependency problems prevent configuration' "$log"; then
    local dep
    dep=$(grep -m1 -A1 'depends on' "$log" | tr '\n' ' ' | cut -c1-200)
    echo "depmissing $dep"
  elif grep -qE 'script subprocess (returned error|failed)|subprocess .* returned error exit status' "$log"; then
    local l
    l=$(grep -m1 -B3 -E '(script|subprocess) .*(returned error|failed with exit)' "$log" |
      grep -v '^dpkg: error processing\|^Setting up\|^Preparing\|^Unpacking' |
      tr '\n' ' ' | cut -c1-300)
    echo "script $l"
  elif grep -q 'error processing archive\|error processing package' "$log"; then
    echo "unpack $(grep -m1 -A2 'error processing' "$log" | tr '\n' ' ' | cut -c1-300)"
  else
    echo "other $(tail -3 "$log" | tr '\n' ' ' | cut -c1-300)"
  fi
}

# Whether output $2 (exit code $1) shows the program actually works.
run_ok() {
  local rc=$1 out=$2
  if grep -qiE 'error while loading shared libraries|bad interpreter|No such file or directory|ModuleNotFoundError|ImportError|cannot load such file' "$out"; then
    return 1
  fi
  [ "$rc" = 0 ] && return 0
  [ "$rc" != 124 ] && [ "$rc" != 126 ] && [ "$rc" != 127 ] &&
    grep -qiE 'usage|version|options|--help' "$out"
}

try_program() {
  local out=$OUT/run.out rc a
  for a in --version --help; do
    timeout -k 2 10 "$@" "$a" </dev/null >"$out" 2>&1; rc=$?
    if run_ok "$rc" "$out"; then echo ok; return; fi
  done
  if grep -qiE 'not a terminal|tty|terminal|curses|TERM' "$out"; then
    echo tty
  elif [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
    echo hang
  else
    echo fail
  fi
}

survey_one() {
  local section=$1 pkg=$2 pool=$3
  local log=$OUT/logs/$pkg.log deb=$OUT/archives/$pkg.deb
  local pfx=$OUT/work/$pkg
  rm -rf "$pfx"
  : > "$log"

  if [ ! -f "$deb" ]; then
    curl -fsSL "$MIRROR/$pool" -o "$deb" >>"$log" 2>&1 || {
      curl -fsSL "$MIRROR/$pool" -o "$deb" >>"$log" 2>&1
    }
  fi
  if [ ! -s "$deb" ]; then
    detail=$(install_failure "$log")
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$section" "$pkg" "${detail%% *}" "${detail#* }" - "" >> "$RES"
    printf '%-16s %-40s %-10s %s\n' "$section" "$pkg" "${detail%% *}" "gone/network" | cut -c1-200
    return
  fi

  DN_PREFIX="$pfx" "$HERE/prototype-install.sh" "$deb" >>"$log" 2>&1
  inst=ok
  if ! DN_PREFIX="$pfx" dpkg-query --admindir="$pfx/var/lib/dpkg" -W -f '${db:Status-Abbrev}' "$pkg" 2>/dev/null | grep -q '^ii'; then
    detail=$(install_failure "$log")
    inst=${detail%% *}
    detail=${detail#* }
    [ "$inst" != "$detail" ] || detail=""
  else
    detail=""
  fi

  run=- progs=""
  if [ "$inst" = ok ]; then
    local ok=0 bad=0 unt=0 n=0 p r
    while IFS= read -r p; do
      [ "$n" -lt "$MAXPROG" ] || break
      full="$pfx/root$p"
      [ -x "$full" ] && [ ! -d "$full" ] || continue
      n=$((n + 1))
      echo "=== $p" >> "$log"
      r=$(try_program "$full")
      cat "$OUT/run.out" >> "$log"
      progs="${progs:+$progs,}${p##*/}:$r"
      case $r in ok) ok=$((ok + 1)) ;; fail) bad=$((bad + 1)) ;; *) unt=$((unt + 1)) ;; esac
    done < <(dpkg --admindir="$pfx/var/lib/dpkg" -L "$pkg" 2>/dev/null | grep -E '^/(usr/)?(s?bin|games)/[^/]+$')
    if [ $((ok + bad + unt)) -eq 0 ]; then run=none
    elif [ $bad -eq 0 ] && [ $ok -gt 0 ]; then run=ok
    elif [ $bad -eq 0 ]; then run=untested
    elif [ $ok -gt 0 ]; then run=partial
    else run=fail; fi
  fi

  detail=$(printf '%s' "$detail" | tr '\t\n' '  ')
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$section" "$pkg" "$inst" "$detail" "$run" "$progs" >> "$RES"
  printf '%-16s %-40s %-10s %-8s %s\n' "$section" "$pkg" "$inst" "$run" "${progs:-$detail}" | cut -c1-200
}

while IFS=$'\t' read -r section pkg pool; do
  case $section in section|'') continue ;; esac
  if cut -f2 "$RES" | grep -qxF -- "$pkg"; then continue; fi
  survey_one "$section" "$pkg" "$pool"
done < "$LIST"
