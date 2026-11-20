#!/bin/sh
# dn-adb -- enter an ADB-hosted prefix at the Android shell uid.
#
#   DN_ADB_PORT=39063 dn-adb [--prefix DIR] [-c 'command']
#
# A connection must exist (or DN_ADB_PORT/--port must be given to connect):
# there is no local fallback, and the prefix must say host=adb.  The prefix
# runs at uid=2000(shell), Seccomp: 0; adb is the transport and the boundary.
set -eu

PORT=${DN_ADB_PORT:-}
PREFIX=${DN_ADB_PREFIX:-/data/local/tmp/p-adb}
CMD=

usage() {
  sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case $1 in
    --port) PORT=$2; shift 2 ;;
    --prefix) PREFIX=$2; shift 2 ;;
    -c|--command) CMD=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "dn-adb: unknown option $1" >&2; exit 2 ;;
    *) CMD=$*; break ;;
  esac
done

command -v adb >/dev/null 2>&1 || { echo "dn-adb: adb not found" >&2; exit 2; }

if [ -n "$PORT" ]; then
  adb connect "localhost:$PORT" >/dev/null 2>&1 || true
fi
DEV=$(adb devices 2>/dev/null | awk 'NR > 1 && $2 == "device" { print $1; exit }')
if [ -z "$DEV" ]; then
  cat >&2 <<'EOF'
dn-adb: no adb device.
Enable Wireless debugging (or USB) and pair once, then:
  adb connect localhost:<port>
or set DN_ADB_PORT=<port> (or pass --port) and run again.
EOF
  exit 1
fi

adb -s "$DEV" shell "[ -x '$PREFIX/boot/boot.sh' ]" >/dev/null 2>&1 || {
  echo "dn-adb: no prefix at $PREFIX (use --prefix or scripts/host/adb-deploy.sh)" >&2
  exit 1
}
HOST=$(adb -s "$DEV" shell "sed -n 's/^host=//p' '$PREFIX/boot/version' 2>/dev/null" | tr -d '\r')
if [ "$HOST" != adb ]; then
  echo "dn-adb: $PREFIX is not an adb-hosted prefix (host='$HOST'; build with --host adb)" >&2
  exit 1
fi

if [ -n "$CMD" ]; then
  # The command is read as a script by the prefix's sh, so quoting stays local.
  printf '%s\n' "$CMD" | adb -s "$DEV" shell "cd '$PREFIX' && ./boot/boot.sh sh"
else
  exec adb -s "$DEV" shell -t "cd '$PREFIX' && exec ./boot/boot.sh"
fi
