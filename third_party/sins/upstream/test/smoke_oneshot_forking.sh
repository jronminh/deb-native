#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SC="$ROOT/build/systemctl"
[[ -x "$SC" ]] || { echo "smoke_oneshot_forking: missing build/systemctl" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/sv" "$TMP/enabled" "$TMP/unit"
cp "$ROOT/test/units/oneshot.service" "$TMP/unit/oneshot.service"
cp "$ROOT/test/units/forking.service" "$TMP/unit/forking.service"

cat >"$TMP/bin/sv" <<'EOSV'
#!/bin/sh
case "$1" in
start|stop|restart|reload|force-reload|hup|term|up|down|exit) exit 0 ;;
status) echo "run: ${2}: (pid 1) 1s" ;;
check) exit 0 ;;
*) exit 0 ;;
esac
EOSV
chmod +x "$TMP/bin/sv"

export PATH="$TMP/bin:$PATH"
export RUNIT_SV_DIR="$TMP/sv"
export RUNIT_SERVICE_DIR="$TMP/enabled"
export SYSTEMD_UNIT_PATH="$TMP/unit"

"$SC" enable oneshot.service >/dev/null
"$SC" enable forking.service >/dev/null

oneshot_run="$TMP/sv/oneshot/run"
forking_run="$TMP/sv/forking/run"
[[ -f "$oneshot_run" && -f "$forking_run" ]]
grep -q "exec sh -c " "$oneshot_run"
grep -q "Type=forking" "$forking_run" || grep -q "PIDFILE=" "$forking_run"
grep -q "PIDFILE='/run/forking-smoke.pid'" "$forking_run"

echo "smoke_oneshot_forking: OK"
