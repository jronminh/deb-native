#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SC="$ROOT/build/systemctl"
[[ -x "$SC" ]] || { echo "smoke_reload: missing build/systemctl" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/sv/reloadable/supervise" "$TMP/enabled" "$TMP/unit"
cp "$ROOT/test/units/reloadable.service" "$TMP/unit/reloadable.service"
rm -f /tmp/sins-reload.log

cat >"$TMP/bin/sv" <<'EOSV'
#!/bin/sh
case "$1" in
status) echo "run: ${2}: (pid 999) 1s" ;;
hup|start|stop|restart|reload|force-reload|term|up|down|exit) exit 0 ;;
check) exit 0 ;;
*) exit 0 ;;
esac
EOSV
chmod +x "$TMP/bin/sv"

export PATH="$TMP/bin:$PATH"
export RUNIT_SV_DIR="$TMP/sv"
export RUNIT_SERVICE_DIR="$TMP/enabled"
export SYSTEMD_UNIT_PATH="$TMP/unit"

"$SC" reload reloadable.service >/dev/null
grep -q "RELOAD" /tmp/sins-reload.log

echo "smoke_reload: OK"
