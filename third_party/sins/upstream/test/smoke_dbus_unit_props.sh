#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DAEMON="$ROOT/build/sins-daemon"
[[ -x "$DAEMON" ]] || { echo "smoke_dbus_unit_props: missing build/sins-daemon" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/sv/nginx/supervise" "$TMP/enabled"
echo "run" >"$TMP/sv/nginx/supervise/stat"

dbus-run-session -- bash -ceu '
  export SINS_SESSION=1
  export RUNIT_SV_DIR="'"$TMP"'/sv"
  export RUNIT_SERVICE_DIR="'"$TMP"'/enabled"
  "'"$DAEMON"'" >/tmp/sins-dbus-daemon.log 2>&1 &
  dpid=$!
  trap "kill -TERM $dpid 2>/dev/null || true; wait $dpid 2>/dev/null || true" EXIT
  sleep 2

  busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager ListUnits >/tmp/sins-dbus-list.out
  grep -q "nginx.service" /tmp/sins-dbus-list.out

  busctl --user get-property org.freedesktop.systemd1 /org/freedesktop/systemd1/unit/nginx org.freedesktop.systemd1.Unit CanStart >/tmp/sins-prop-canstart.out
  busctl --user get-property org.freedesktop.systemd1 /org/freedesktop/systemd1/unit/nginx org.freedesktop.systemd1.Unit ActiveState >/tmp/sins-prop-active.out
  grep -q "^b true$" /tmp/sins-prop-canstart.out
  grep -q "^s \"active\"$" /tmp/sins-prop-active.out

  # Invalid unit name must return D-Bus error (manager must not fake success).
  if busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager StartUnit ss invalid/name.service replace >/tmp/sins-start.out 2>&1; then
    echo "expected StartUnit with invalid name to fail" >&2
    exit 1
  fi
'

echo "smoke_dbus_unit_props: OK"
