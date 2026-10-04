#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SC="$ROOT/build/systemctl"
[[ -x "$SC" ]] || { echo "smoke_template_units: missing build/systemctl" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/sv" "$TMP/enabled" "$TMP/unit"
cp "$ROOT/test/units/template@.service" "$TMP/unit/sins-tpl@.service"

cat >"$TMP/bin/sv" <<'EOSV'
#!/bin/sh
case "$1" in
start|stop|restart|reload|force-reload|hup|term|up|down|exit) exit 0 ;;
status) echo "run: ${2}: (pid 123) 1s" ;;
check) exit 0 ;;
*) exit 0 ;;
esac
EOSV
chmod +x "$TMP/bin/sv"

export PATH="$TMP/bin:$PATH"
export RUNIT_SV_DIR="$TMP/sv"
export RUNIT_SERVICE_DIR="$TMP/enabled"
export SYSTEMD_UNIT_PATH="$TMP/unit"

"$SC" enable sins-tpl@alpha.service >/dev/null
run_path="$TMP/sv/sins-tpl@alpha/run"
[[ -f "$run_path" ]]
grep -q "TPL-alpha-sins-tpl-sins-tpl@.service" "$run_path"
if grep -q '%i\|%p\|%n' "$run_path"; then
  echo "template placeholders not replaced in run script" >&2
  exit 1
fi

# daemon-reload should keep template placeholder replacement on existing instance.
"$SC" daemon-reload >/dev/null
grep -q "TPL-alpha-sins-tpl-sins-tpl@.service" "$run_path"
echo "smoke_template_units: OK"
