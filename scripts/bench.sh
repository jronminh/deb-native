#!/bin/sh
# A second bench beside the Lua build: real workloads, each its own run of the
# runtime, so the stops attribute to one workload and the wall is comparable.
# It aims at the axes the Lua build does not touch: a stat storm, links and
# missing names, a git workflow's renames and lock files, Python's os calls,
# and a tar round trip.
#
#   bench.sh PREFIX [workload ...]
#
# Prints wall time and the tracer's entry/exit stops. Interleave runs: the
# phone's timings vary 10-20% between runs.

set -u
P=$(cd "${1:?usage: bench.sh PREFIX [workload ...]}" && pwd); shift
DN=$P/usr/lib/deb-native/dn-tiny
[ -x "$DN" ] || { echo "no runtime at $DN" >&2; exit 1; }

run() {
	t0=$(date +%s%N)
	out=$("$DN" -t "$P" -- /usr/bin/bash /boot/init.sh \
		/usr/bin/sh -c "$2" 2>&1 >/dev/null)
	t1=$(date +%s%N)
	# awk: /bin/sh here is Android's, whose arithmetic is 32-bit
	ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%d", (b - a) / 1000000 }')
	printf '%-6s %7s ms  %s\n' "$1" "$ms" \
		"$(printf '%s' "$out" | sed -n 's/.*\(enter=[0-9]* exit=[0-9]*\).*/\1/p' | tail -1)"
}

all='find ls links spawn git py tar'
for w in ${*:-$all}; do
	case $w in
	find)  run find  'find /usr -type f > /dev/null' ;;
	ls)    run ls    'ls -lR /usr/share > /dev/null 2>&1' ;;
	links) run links 'find /usr -type l -printf "%p -> %l\n" > /dev/null; stat -L -c %N /usr/bin/ls /nonexistent > /dev/null 2>&1; realpath /usr/bin/ls /usr/lib/* > /dev/null' ;;
	spawn) run spawn 'i=0; while [ $i -lt 300 ]; do /usr/bin/true; i=$((i+1)); done' ;;
	git)  run git  'rm -rf /tmp/bg; git -c init.defaultBranch=main init -q /tmp/bg; cd /tmp/bg; cp -a /usr/share/ca-certificates/. . 2>/dev/null; git add -A; git -c user.email=a@b -c user.name=a commit -qm x; git status > /dev/null' ;;
	py)   run py   'python3 -c "import os
for r, d, f in os.walk(\"/usr/lib/python3.13\"):
    for n in d + f:
        try: os.stat(os.path.join(r, n))
        except OSError: pass"' ;;
	tar)  run tar  'rm -rf /tmp/bt; mkdir -p /tmp/bt; tar czf /tmp/b.tgz -C /usr/share/ca-certificates . && tar xzf /tmp/b.tgz -C /tmp/bt' ;;
	*) echo "unknown workload: $w" >&2 ;;
	esac
done
