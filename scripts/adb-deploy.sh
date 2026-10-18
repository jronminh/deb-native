#!/bin/sh
# Deploy a prefix tarball over ADB and boot it at the Android shell uid.
#
#   scripts/adb-deploy.sh [--port PORT] [--dest DIR] [--no-run] PREFIX.tar.gz
#
# The prefix must be built with `build-prefix.sh --host adb`: it carries the
# adb runtime (no app-domain seccomp answers) and `boot/version` says host=adb.
# The prefix is extracted under /data/local/tmp (persistent, shell-owned) and
# run through `adb shell`; this is not a sandbox -- the shell uid is a serious
# privilege tier, so only deploy code you trust.
set -eu

PORT= DEST= RUN=1
while [ $# -gt 0 ]; do
  case $1 in
    --port) PORT=$2; shift 2 ;;
    --dest) DEST=$2; shift 2 ;;
    --no-run) RUN=0; shift ;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "adb-deploy: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 1 ] || { echo "usage: adb-deploy.sh [--port PORT] [--dest DIR] [--no-run] PREFIX.tar.gz" >&2; exit 2; }
TAR=$1
[ -f "$TAR" ] || { echo "adb-deploy: no $TAR" >&2; exit 2; }
command -v adb >/dev/null 2>&1 || { echo "adb-deploy: adb not found" >&2; exit 2; }

BASE=$(basename "$TAR")
NAME=${BASE%.tar.gz}
[ "$NAME" = "$BASE" ] && NAME=${BASE%.tgz}
[ -n "$DEST" ] || DEST=/data/local/tmp/$NAME
TMP=/data/local/tmp/$BASE

if [ -n "$PORT" ]; then
  adb connect "localhost:$PORT" >/dev/null 2>&1 || true
fi
adb wait-for-device >/dev/null
[ "$(adb get-state 2>/dev/null)" = device ] ||
  { echo "adb-deploy: no device (pair/connect first; --port takes the Wireless debugging port)" >&2; exit 2; }

echo "adb-deploy: $TAR -> $DEST"
adb push "$TAR" "$TMP" >/dev/null
adb shell "rm -rf '$DEST' && mkdir -p '$DEST' && tar xzf '$TMP' -C '$DEST'" >/dev/null
adb shell "rm -f '$TMP'"

HOST=$(adb shell "sed -n 's/^host=//p' '$DEST/boot/version' 2>/dev/null" | tr -d '\r')
[ "$HOST" = adb ] ||
  { echo "adb-deploy: $TAR was not built with --host adb (boot/version host='$HOST')" >&2; exit 2; }

UID=$(adb shell 'id -u' | tr -d '\r')
echo "adb-deploy: extracted as uid $UID ($(adb shell 'id -un' | tr -d '\r'))"
if [ "$RUN" = 1 ]; then
  echo "adb-deploy: booting (first boot runs init.sh and reconciles the real uid)"
  adb shell "cd '$DEST' && ./boot/boot.sh sh -c 'id; echo HOME=\$HOME; pwd'"
fi
echo "run it later:  adb shell 'cd $DEST && ./boot/boot.sh'"
