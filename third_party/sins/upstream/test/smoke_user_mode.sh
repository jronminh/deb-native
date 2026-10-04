#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SC="$ROOT/build/systemctl"
[[ -x "$SC" ]] || { echo "smoke_user_mode: missing build/systemctl" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/home/.config/systemd/user" "$TMP/bin" "$TMP/sv/nginx/supervise" "$TMP/enabled"

cat >"$TMP/home/.config/systemd/user/user-smoke.service" <<'EOU'
[Unit]
Description=User unit smoke

[Service]
ExecStart=/bin/true
EOU

cat >"$TMP/bin/sv" <<'EOSV'
#!/bin/sh
case "$1" in
status) echo "run: ${2}: (pid 1) 1s" ;;
start|stop|restart|reload|force-reload|hup|term|up|down|exit) exit 0 ;;
check) exit 0 ;;
*) exit 0 ;;
esac
EOSV
chmod +x "$TMP/bin/sv"

export PATH="$TMP/bin:$PATH"
export HOME="$TMP/home"
export RUNIT_SV_DIR="$TMP/sv"
export RUNIT_SERVICE_DIR="$TMP/enabled"

"$SC" --user cat user-smoke.service | grep -q "ExecStart=/bin/true"
"$SC" --user show user-smoke.service | grep -q "^Id=user-smoke.service"

if "$SC" --user start user-smoke.service >/tmp/sins-user-start.out 2>&1; then
  echo "smoke_user_mode: expected --user start to fail" >&2
  exit 1
fi
grep -q 'not supported with --user' /tmp/sins-user-start.out

echo "smoke_user_mode: OK"
