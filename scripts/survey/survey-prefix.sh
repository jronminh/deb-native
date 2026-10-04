#!/usr/bin/env bash
# survey-prefix.sh -- install and run a list of Debian packages in the 0.2.0
# prefix, each from the same fresh state, and record where each one fails:
# while installing or while running, and why. sudo-less's dev/survey.sh
# (its method, classifiers and columns) ported to deb-native; the result is
# docs/log/survey-0.2.0.md. (The 0.1.x pipeline's own survey.sh is gone;
# see docs/spec/classic-design.md.)
#
#   OUT=~/survey scripts/survey-prefix.sh docs/log/survey-0.2.0/list.tsv
#
# Needs an installed prefix (install.sh) at DN (default ~/.dn). Its state is
# saved once to OUT/base.tar and restored before every package, so one
# broken package cannot affect the next; the prefix is back to that state
# at the end. Downloaded .debs are shared in OUT/archives. Resumable: a
# package already in OUT/results.tsv is skipped. One survey per prefix (a
# lock beside it); stop one with `kill PID` (the pid is in the lock file):
# it restores the prefix before exiting. Never move OUT while it runs.
#
# LIST.tsv: "section<TAB>package" lines (header "section..." skipped),
# made by scripts/survey-sample.py.
#
# OUT/results.tsv, one line per package:
#   section package install detail run programs seconds
# install: ok, skew (apt cannot satisfy the dependencies), script (a
#   maintainer script failed), unpack, network, gone (not in the archive),
#   other
# run: none (no program in a bin dir), ok, partial, fail, untested (every
#   program needs a display or a terminal, or hangs); "-" if not installed
# programs: NAME:HOW:RESULT,... HOW is how the user's shell reaches it
#   (usr/lib/deb-native/bin, make-launchers.sh): native (a symlink: the prefix loader or
#   a translated "#!"), trace (a dn-run --trace wrapper), dn-run, script
#   (dn-shell/dn-perl wrapper), hidden (no entry; run by full path).
#   RESULT is ok, fail, miss (failed, but works under the tracer: the
#   launcher should have traced it), gui, tty or hang.
# OUT/logs/PKG.log keeps the apt output and each program's output.
set -uo pipefail

LIST=${1:?usage: OUT=DIR $0 LIST.tsv}
: "${OUT:?OUT: a directory for results}"
DN=${DN:-$HOME/.dn}
TP=${PREFIX:-/data/data/com.termux/files/usr}
MAXPROG=${MAXPROG:-6}

[ -x "$DN/usr/bin/apt-get" ] && [ -d "$DN/var/lib/dpkg" ] ||
  { echo "E: no installed prefix at $DN (run install.sh first)" >&2; exit 1; }
case $DN in /*/.dn|/*/.dn/) ;; *)
  echo "E: DN=$DN: the survey deletes and restores it, so it must be a */.dn path" >&2; exit 1 ;;
esac
DN=${DN%/}

mkdir -p "$OUT/logs" "$OUT/archives/partial" "$OUT/home"
OUT=$(cd "$OUT" && pwd)
RES=$OUT/results.tsv
BASE=$OUT/base.tar
[ -s "$RES" ] || printf 'section\tpackage\tinstall\tdetail\trun\tprograms\tseconds\n' > "$RES"

# One survey per prefix: a second run would delete the prefix under the
# first. The lock sits beside the prefix, not in OUT, so two OUTs clash too.
LOCK=${DN%/*}/.dn-survey.lock
if [ -f "$LOCK" ] && [ -d "/proc/$(cat "$LOCK")" ]; then
  echo "E: a survey is already running (pid $(cat "$LOCK"), $LOCK); stop it with: kill $(cat "$LOCK")" >&2
  exit 1
fi
echo $$ > "$LOCK"

if [ ! -s "$BASE" ]; then
  echo "Saving the prefix to $BASE ..."
  tar -C "${DN%/*}" -cf "$BASE" --exclude="${DN##*/}/var/cache/apt/archives/*.deb" "${DN##*/}"
fi

# The prefix is deleted only when the snapshot to rebuild it is there.
restore() {
  [ -s "$BASE" ] || { echo "E: $BASE is gone; the prefix at $DN is left as it is" >&2; exit 1; }
  rm -rf "$DN"
  tar -C "${DN%/*}" -xf "$BASE"
}

# `kill PID` (the lock's pid) stops cleanly: the running install is
# stopped, the prefix restored, the lock removed.
killtree() {
  local c
  for c in $(pgrep -P "$1"); do killtree "$c"; done
  kill -TERM "$1" 2>&1 | grep -v 'No such process'
}
stop() {
  trap - TERM INT
  local c
  for c in $(pgrep -P $$); do killtree "$c"; done
  wait
  restore
  rm -f "$LOCK"
  echo "Stopped; the prefix is restored."
  exit 130
}
trap stop TERM INT

# No desktop session: Termux:X11 or VNC may be running.
headless() {
  unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS QT_QPA_PLATFORM GDK_BACKEND
  export HOME=$OUT/home
}

# Why apt-get install failed, from its output in $1 (sudo-less's rules).
install_failure() {
  local log=$1 l
  if grep -q 'Unable to locate package\|has no installation candidate' "$log"; then
    echo "gone"
  elif grep -q 'Failed to fetch\|Temporary failure resolving\|Connection timed out' "$log"; then
    echo "network $(grep -m1 -o 'Failed to fetch [^ ]*' "$log")"
  elif grep -qi 'held broken packages\|unmet dependencies\|Unable to correct problems\|Unable to satisfy dependencies' "$log"; then
    l=$(grep -m1 -A1 'is not selected for install because' "$log" |
      sed 's/^ *//; s/^[0-9]*\. //' | paste -sd' ')
    [ -n "$l" ] || l=$(grep -m1 -E 'Depends: .*(but|is not)' "$log" | sed 's/^ *//')
    echo "skew ${l:-$(grep -m1 'Unable to correct\|Unmet' "$log")}"
  elif grep -qE 'script subprocess (returned error|failed)|subprocess .* returned error exit status' "$log"; then
    l=$(grep -m1 -E '(script|subprocess) .*(returned error|failed with exit)' "$log" -B6 |
      grep -v '^dpkg: error processing\|^Setting up\|^Preparing\|^Unpacking' |
      tr '\n' ' ' | tr -s ' ' | cut -c1-300)
    echo "script $l"
  elif grep -q 'error processing archive' "$log"; then
    echo "unpack $(grep -m1 -A2 'error processing archive' "$log" | tr '\n' ' ' | cut -c1-300)"
  else
    echo "other $(grep -m1 '^E: ' "$log" || tail -3 "$log" | tr '\n' ' ' | cut -c1-300)"
  fi
}

# Whether the output in $2 of a run that exited with $1 shows it works.
run_ok() {
  local rc=$1 out=$2
  if grep -qiE 'error while loading shared libraries|bad interpreter|No such file or directory|ModuleNotFoundError|ImportError|Can.t locate .* in @INC|cannot load such file|LoadError|ClassNotFoundException|Could not find or load main class|Cannot find module|not found in (the )?(path|search)|CANNOT LINK EXECUTABLE|Bad system call|Segmentation fault|^dn-run:' "$out"; then
    return 1
  fi
  [ "$rc" = 0 ] && return 0
  # Many programs exit non-zero on --version/--help but print a usage.
  [ "$rc" != 124 ] && [ "$rc" != 126 ] && [ "$rc" != 127 ] && [ "$rc" -lt 128 ] &&
    grep -qiE 'usage|version|options|--help' "$out"
}

# Run command "$@" with --version, then --help. Prints ok, fail, gui, tty or hang.
try_program() {
  local out=$OUT/run.out rc a
  for a in --version --help; do
    timeout -k 2 10 "$@" "$a" </dev/null >"$out" 2>&1; rc=$?
    if run_ok "$rc" "$out"; then echo ok; return; fi
  done
  if grep -qiE 'cannot open display|could not connect to display|no display|qt\.qpa|Gtk-WARNING|Failed to initialize GTK|WAYLAND_DISPLAY|DISPLAY' "$out"; then
    echo gui
  elif grep -qiE 'not a terminal|tty|terminal|curses|TERM' "$out"; then
    echo tty
  elif [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
    echo hang
  else
    echo fail
  fi
}

# How the user's shell reaches program $1 (see the header).
how_reached() {
  local e=$DN/usr/lib/deb-native/bin/$1
  if [ -L "$e" ]; then echo native
  elif [ ! -f "$e" ]; then echo hidden
  elif grep -q -- '--trace' "$e"; then echo trace
  elif grep -q 'dn-run' "$e"; then echo dn-run
  else echo script; fi
}

survey_one() {
  local section=$1 pkg=$2 log=$OUT/logs/$2.log inst detail="" run=none progs="" p n how r
  local ok=0 bad=0 unt=0 t0=$SECONDS
  restore
  (
    headless
    timeout 1800 "$DN/usr/bin/apt-get" install -y --no-install-recommends \
      -o Dir::Cache::Archives="$OUT/archives" "$pkg"
  ) >"$log" 2>&1 &
  # In the background and waited for, so `kill` reaches stop() at once.
  wait $!
  local rc=$?
  if [ $rc = 0 ] &&
     "$DN/usr/bin/dpkg-query" -W -f '${db:Status-Abbrev}' "$pkg:arm64" 2>&1 | grep -q '^ii'; then
    inst=ok
  else
    detail=$(install_failure "$log"); inst=${detail%% *}
    [ "$inst" != "$detail" ] && detail=${detail#* } || detail=""
  fi

  if [ "$inst" = ok ]; then
    n=0
    while IFS= read -r p; do
      [ $n -lt "$MAXPROG" ] || break
      [ -x "$DN$p" ] && [ ! -d "$DN$p" ] || continue
      n=$((n + 1))
      how=$(how_reached "${p##*/}")
      r=$(
        headless
        export PATH=$DN/usr/lib/deb-native/bin:$TP/bin
        echo "=== ${p##*/} ($how)" >>"$log"
        if [ "$how" = hidden ]; then r=$(try_program "$DN$p"); else r=$(try_program "${p##*/}"); fi
        cat "$OUT/run.out" >>"$log"
        if [ "$how" != trace ] && [ "$r" = fail ]; then
          r2=$(try_program "$DN/usr/lib/deb-native/dn-run" --trace "$DN$p")
          echo "=== ${p##*/} (retry under the tracer: $r2)" >>"$log"
          cat "$OUT/run.out" >>"$log"
          [ "$r2" != ok ] || r=miss
        fi
        echo "$r"
      )
      progs+="${progs:+,}${p##*/}:$how:$r"
      case $r in ok) ok=$((ok + 1)) ;; fail|miss) bad=$((bad + 1)) ;; *) unt=$((unt + 1)) ;; esac
    done < <("$DN/usr/bin/dpkg-query" -L "$pkg:arm64" 2>&1 |
               grep -E '^/(usr/)?(s?bin|games)/[^/]+$')
    if [ $((ok + bad + unt)) -eq 0 ]; then run=none
    elif [ $bad -eq 0 ] && [ $ok -gt 0 ]; then run=ok
    elif [ $bad -eq 0 ]; then run=untested
    elif [ $ok -gt 0 ]; then run=partial
    else run=fail; fi
  else
    run=-
  fi
  detail=$(printf '%s' "$detail" | tr '\t\n' '  ')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$section" "$pkg" "$inst" "$detail" "$run" "$progs" "$((SECONDS - t0))" >> "$RES"
  printf '%-13s %-34s %-6s %-8s %s\n' "$section" "$pkg" "$inst" "$run" "${progs:-$detail}" | cut -c1-160
}

# Read once: a git pull that rewrites LIST must not change a running survey.
mapfile -t ENTRIES < "$LIST"
for e in "${ENTRIES[@]}"; do
  IFS=$'\t' read -r section pkg _ <<< "$e"
  case $section in section|'') continue ;; esac
  if cut -f2 "$RES" | grep -qxF -- "$pkg"; then continue; fi   # resumable
  survey_one "$section" "$pkg"
done
restore
rm -f "$LOCK"
echo "Done: $RES"
