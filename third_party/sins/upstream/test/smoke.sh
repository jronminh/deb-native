#!/usr/bin/env bash
# Integration smoke: journal .so, systemctl with mocked sv + dirs, optional D-Bus.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

need() {
  echo "smoke: missing $1 (run: SINS_CHOICE=0 ./build.sh && ./build.sh --verify)" >&2
  exit 2
}

[[ -f build/libsystemd.so.0 ]] || need build/libsystemd.so.0
[[ -f build/systemctl ]] || need build/systemctl

echo "smoke: journal_smoke"
gcc -Wall -Wextra -o /tmp/sins-journal_smoke test/journal_smoke.c \
  build/libsystemd.so.0 -Wl,-rpath,"$ROOT/build" -pthread -ldl
/tmp/sins-journal_smoke

echo "smoke: systemctl (mock sv + temp dirs)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/sv/nginx" "$TMP/unit" "$TMP/enabled"
cat >"$TMP/bin/sv" <<'EOSV'
#!/bin/sh
case "$1" in
start|stop|restart|reload|force-reload|once|term|up|down|exit)
	exit 0
	;;
status)
	echo "run: ${2}: (pid 1) 1s"
	;;
check)
	exit 0
	;;
*)
	exit 0
	;;
esac
EOSV
chmod +x "$TMP/bin/sv"
printf '%s\n' '#!/bin/sh' 'exec sleep 3600' >"$TMP/sv/nginx/run"
chmod +x "$TMP/sv/nginx/run"
cat >"$TMP/unit/nginx.service" <<'EOSVC'
[Unit]
Description=Nginx smoke unit

[Service]
ExecStart=/bin/true
Environment=SMOKE=1
EOSVC
export PATH="$TMP/bin:$PATH"
export RUNIT_SV_DIR="$TMP/sv"
export RUNIT_SERVICE_DIR="$TMP/enabled"
export SYSTEMD_UNIT_PATH="$TMP/unit"
SC="$ROOT/build/systemctl"
"$SC" list-units | grep -q nginx
"$SC" list-unit-files | grep -q nginx.service
"$SC" cat nginx.service | grep -q ExecStart
"$SC" show nginx.service | grep -q '^Id='

if command -v dbus-run-session >/dev/null 2>&1 && [[ -x "$ROOT/build/sins-daemon" ]]; then
	echo "smoke: dbus (session)"
	dbus-run-session -- bash "$ROOT/test/smoke_dbus.sh" "$ROOT/build/sins-daemon"
else
	echo "smoke: skip dbus (need dbus-run-session and build/sins-daemon)"
fi

echo "smoke: OK"
