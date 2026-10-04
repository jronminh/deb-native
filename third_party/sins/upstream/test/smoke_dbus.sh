#!/usr/bin/env bash
# Session-bus smoke for sins-daemon.
set -euo pipefail

DAEMON="${1:?path to sins-daemon}"
export SINS_SESSION=1
"$DAEMON" &
dpid=$!
cleanup() {
  kill -TERM "$dpid" 2>/dev/null || true
  wait "$dpid" 2>/dev/null || true
}
trap cleanup EXIT

sleep 2
tmp="$(mktemp)"
busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager ListUnits >"$tmp"
grep -q 'a(ssssssouso)' "$tmp"
rm -f "$tmp"
