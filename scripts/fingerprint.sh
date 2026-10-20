#!/bin/sh
# The fingerprint test (docs/spec/phase2.md): the kernel and the hardware the
# prefix shows must be the host's own (the truth), the runtime's traces must be
# gone, and the Debian identity must be the tree's.
#
#   scripts/fingerprint.sh PREFIX
set -u
P=$(CDPATH= cd -- "${1:?usage: fingerprint.sh PREFIX}" && pwd)
DN=$P/usr/lib/deb-native/dn-init
[ -x "$DN" ] || { echo "no runtime at $DN" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cp "$HERE/fingerprint/probe.sh" "$P/tmp/dn-fingerprint.sh" || exit 1
out=$(DN_INITIAL=root "$P/boot/boot.sh" sh /tmp/dn-fingerprint.sh 2>&1)
printf '%s\n' "$out"
rm -f "$P/tmp/dn-fingerprint.sh"

g() { printf '%s\n' "$out" | sed -n "s/^$1=//p" | head -1; }
fail=0
expect() {
	if [ "$2" = "$3" ]; then
		echo "ok   $1 = $2"
	else
		echo "FAIL $1 = '$2' (want '$3')"
		fail=1
	fi
}

echo
echo "== fingerprint check =="
expect kernel   "$(g uname_s)" "Linux"
expect release  "$(g uname_r)" "$(uname -r)"      # real kernel: the host's
expect machine  "$(g uname_m)" "$(uname -m)"      # real hardware: the host's
expect uid      "$(g uid)" "0"                    # fake root
expect arch     "$(g arch)" "arm64"
expect os_id    "$(g os_id)" "debian"
# the runtime's traces must be gone
expect tracerpid "$(g tracerpid)" "0"
sc=$(g seccomp)
if [ -z "$sc" ] || [ "$sc" = "0" ]; then echo "ok   seccomp = '${sc}' (none)"; else echo "FAIL seccomp = '$sc'"; fail=1; fi
st=$(g state)
case "$st" in
	R*) echo "ok   state = '$st'" ;;
	*) echo "FAIL state = '$st' (not running)"; fail=1 ;;
esac
nm=$(g name)
case "$nm" in
	*ld-linux*|*dn-init*|*dn-lane*) echo "FAIL name = '$nm' (the runtime)"; fail=1 ;;
	'') echo "FAIL name is empty"; fail=1 ;;
	*) echo "ok   name = '$nm'" ;;
esac
if printf '%s\n' "$out" | grep -qi android; then echo "FAIL an 'android' string is shown"; fail=1; else echo "ok   no 'android' string"; fi

echo
if [ "$fail" = 0 ]; then echo "fingerprint: clean"; else echo "fingerprint: differences above"; exit 1; fi
