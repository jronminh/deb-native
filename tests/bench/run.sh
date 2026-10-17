#!/bin/sh
# The bench and check set of docs/performance.md, "In an app session".
#
#   tests/bench/run.sh PREFIX [bench|check|all]
#
# bench  the regression set, the file-tree set, find, ls and (with LUA_DIR)
#        the Lua build; each row is the wall and the tracer's entry/exit stops
# check  fr.sh (ids, stat, setresuid, vfork/fork), mt (threads and vfork),
#        xm (execmod/execmem), syscalls (a killed number with no row), owner
#        (chown kept in user.dn.owner), and t.sh (git init and apt-get clean)
#
# LUA_DIR is a guest path (default: /home/bench/lua-5.4.7, skipped if absent).
# The tracer polls 500 us, as boot.sh does.  Interleave runs before and after a
# runtime change: timings vary 10-20% between runs.
set -u

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; }
[ $# -ge 1 ] || { usage >&2; exit 2; }
P=$(CDPATH= cd -- "$1" && pwd) || exit 2
shift
SUITE=${1:-all}
DN=$P/usr/lib/deb-native/dn-tiny
[ -x "$DN" ] || { echo "run.sh: no runtime at $DN" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KIT=$P/tmp/dn-bench
mkdir -p "$KIT"
cp "$HERE"/reg.sh "$HERE"/fr.sh "$HERE"/lua.sh "$HERE"/t.sh "$HERE"/exec200.sh "$KIT"/ || exit 1
cp "$HERE"/mt.c "$HERE"/xm.c "$HERE"/syscalls.c "$KIT"/ || exit 1
cp "$HERE"/owner.sh "$KIT"/ || exit 1
cd "$P" || exit 1                        # the tracee's cwd is then the tree's /

quiet() { "$DN" -q -s 500 -t "$P" -- /usr/bin/bash /boot/init.sh /usr/bin/sh -c "$1"; }

timed() {
	t0=$(date +%s%N)
	out=$("$DN" -s 500 -t "$P" -- /usr/bin/bash /boot/init.sh /usr/bin/sh -c "$2" 2>&1 >/dev/null)
	t1=$(date +%s%N)
	ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%d", (b - a) / 1000000 }')
	stops=$(printf '%s' "$out" | sed -n 's/.*\(enter=[0-9]* exit=[0-9]*\).*/\1/p' | tail -1)
	printf '%-22s %7s ms  %s\n' "$1" "$ms" "$stops"
}

bench() {
	timed reg  'sh /tmp/dn-bench/reg.sh run'
	(cd "$P/tmp/reg-run" 2>/dev/null && md5sum * 2>/dev/null) | md5sum |
		sed 's/^/  reg digest: /'
	timed tree 'find /usr -type f >/dev/null; dpkg -l >/dev/null; apt-cache show bash >/dev/null; ls -lR /usr/share/doc >/dev/null 2>&1'
	timed find 'find /usr -type f >/dev/null'
	timed ls   'ls -lR /usr/share >/dev/null 2>&1'
	if [ -n "${LUA_DIR:-}" ]; then
		timed lua "LUA_DIR=$LUA_DIR sh /tmp/dn-bench/lua.sh"
	else
		printf '%-22s %7s    %s\n' lua - 'set LUA_DIR (a guest path) for the Lua build'
	fi
}

check() {
	fail=0
	echo "== fr.sh (ids, stat, setresuid, vfork/fork)"
	quiet 'sh /tmp/dn-bench/fr.sh' || fail=1

	echo "== mt (threads, vfork, fork)"
	o=$(quiet 'gcc -O2 -pthread -o /tmp/dn-bench/mt /tmp/dn-bench/mt.c && /tmp/dn-bench/mt') || fail=1
	printf '%s\n' "$o"
	case "$o" in
	*threads:\ 0\ wrong*"vfork child rc=0"*"fork child cwd ok=1"*) ;;
	*) echo "   mt: FAIL"; fail=1 ;;
	esac

	echo "== xm (execmod, execmem; the checks Android 16 needs)"
	o=$(quiet 'gcc -O2 -o /tmp/dn-bench/xm /tmp/dn-bench/xm.c && /tmp/dn-bench/xm')
	rc=$?
	printf '%s\n' "$o"
	if [ "$rc" != 0 ]; then echo "   xm: FAIL (rc=$rc)"; fail=1; fi

	echo "== syscalls (the Android 16 denials with no row answer ENOSYS)"
	o=$(quiet 'gcc -O2 -o /tmp/dn-bench/syscalls /tmp/dn-bench/syscalls.c && /tmp/dn-bench/syscalls') || fail=1
	printf '%s\n' "$o"
	case "$o" in
	*"survived 14 killed numbers"*) ;;
	*) echo "   syscalls: FAIL"; fail=1 ;;
	esac

	echo "== owner (chown kept in user.dn.owner, answered by stat)"
	o=$(quiet 'sh /tmp/dn-bench/owner.sh') || fail=1
	printf '%s\n' "$o"
	case "$o" in
	*"owner: ok"*) ;;
	*) echo "   owner: FAIL"; fail=1 ;;
	esac

	echo "== t.sh (git init, apt-get clean)"
	o=$(quiet 'sh /tmp/dn-bench/t.sh') || fail=1
	printf '%s\n' "$o"
	case "$o" in *GIT_OK*APT_OK*) ;; *) echo "   t.sh: FAIL"; fail=1 ;; esac

	[ "$fail" = 0 ] && echo "checks: ok" || echo "checks: FAILED"
	return "$fail"
}

case $SUITE in
bench) bench ;;
check) check ;;
all) bench; echo; check ;;
*) echo "run.sh: unknown suite '$SUITE' (bench, check, all)" >&2; usage >&2; exit 2 ;;
esac
