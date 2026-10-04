#!/bin/bash
# SINS — modular build (no systemd/libsystemd required to compile).
#
# Minimal / Void / headless Artix: pick only what you need.
#
# Env (precedence: SINS_TAGS > CLI --tags > SINS_CHOICE > SINS_PROFILE > default):
#   SINS_TAGS       exact Go tags, comma-separated (e.g. dbus,notify)
#   SINS_PROFILE    minimal | dbus | de | server | full
#   SINS_CHOICE     legacy: 0=all, or 1,2,3,4,5 = dbus,sockets,timers,notify,cgroups
#   SINS_STRIP=1    strip binaries + .so after build (smaller installs)
#   SINS_SYNC_STUB=1  regenerate stub jumps (needs Python 3)
#
# Non-interactive default: minimal (systemctl + libsystemd only). Set SINS_PROFILE or SINS_CHOICE for more.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

mkdir -p build

can_build_stub() {
	[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]]
}

install_service() {
	echo "Installing sins-daemon as runit service..."
	
	if [[ "$EUID" -ne 0 ]]; then
		echo "Error: --install requires root (use sudo)" >&2
		exit 1
	fi
	
	# Detect service directory
	local sv_dir="/etc/runit/sv"
	local service_dir="/var/service"
	
	if [[ -d "/etc/runit/sv" ]]; then
		sv_dir="/etc/runit/sv"
	elif [[ -d "/etc/sv" ]]; then
		sv_dir="/etc/sv"
	fi
	
	if [[ -d "/run/runit/service" ]]; then
		service_dir="/run/runit/service"
	elif [[ -d "/var/service" ]]; then
		service_dir="/var/service"
	fi
	
	# Create service directory
	mkdir -p "$sv_dir/sins-daemon"
	
	# Create run script
	cat > "$sv_dir/sins-daemon/run" <<'RUNEOF'
#!/bin/sh
exec 2>&1
exec sins-daemon
RUNEOF
	chmod +x "$sv_dir/sins-daemon/run"
	
	# Create finish script for cleanup
	cat > "$sv_dir/sins-daemon/finish" <<'FINEOF'
#!/bin/sh
# sins-daemon cleanup
FINEOF
	chmod +x "$sv_dir/sins-daemon/finish"
	
	# Enable service
	if [[ -d "$service_dir" ]]; then
		ln -sf "$sv_dir/sins-daemon" "$service_dir/" 2>/dev/null || \
			echo "Warning: could not enable service (may already exist)"
	fi
	
	echo "sins-daemon installed to $sv_dir/sins-daemon/"
	echo "Service ${enabled:+enabled}${enabled:+ at $service_dir/sins-daemon}"
	echo ""
	echo "To start now: sv start sins-daemon"
	echo "To check status: sv status sins-daemon"
}

build_stub() {
	echo "Building libsystemd stub (Linux x86_64)..."
	gcc -shared -fPIC \
		-Wall -Wextra -Wno-unused-parameter \
		-pthread \
		-Wl,-soname,libsystemd.so.0 \
		-Wl,--version-script=pkg/libsystemd/libsystemd.map \
		-o build/libsystemd.so.0 pkg/libsystemd/stub.c pkg/libsystemd/journal.c -ldl
}

usage() {
	cat <<EOF
SINS — modular build (no systemd required to compile).

Usage: $0 [options]

  --help, -h          This text
  --verify            Compile check (full tags + no tags + stub when x86_64 Linux)
  --list-profiles     Show named profiles (minimal, dbus, de, server, full)
  --print-tags        Print resolved comma-separated tags and exit (for scripts)
  --install           Install sins-daemon as runit service (requires root)
  --minimal           Same as --profile minimal
  --full, --all       Same as --profile full
  --profile NAME      minimal | dbus | de | server | full
  --tags LIST         Exact Go tags (e.g. dbus,notify)

Env (CLI --tags/--profile wins over these when set):
  SINS_TAGS, SINS_PROFILE, SINS_CHOICE (legacy 0=all, 1–5 combinable), SINS_STRIP=1, SINS_SYNC_STUB=1

Non-interactive default: minimal. TTY with no env: short interactive menu.

Examples:
  ./build.sh --profile minimal
  ./build.sh --profile de
  SINS_TAGS=dbus,timers ./build.sh
  ./build.sh --print-tags --profile server
EOF
}

list_profiles() {
	cat <<'EOF'
Profile     Go tags                           Binaries (typical)
---------   ------------------------------    ---------------------------------
minimal     (none)                            systemctl, libsystemd.so.0
dbus        dbus                              + sins-daemon
de          dbus,notify                       + sins-daemon (desktop session)
server      dbus,timers,cgroups               + sins-daemon, sins-timers
full        dbus,notify,cgroups,timers,sockets + all of the above + sins-sockets

Legacy SINS_CHOICE: 0 = full; 1=dbus 2=sockets 3=timers 4=notify 5=cgroups (combinable, e.g. 1,4)
EOF
}

profile_to_tags() {
	case "$1" in
	minimal) echo "" ;;
	dbus) echo "dbus" ;;
	de | desktop) echo "dbus,notify" ;;
	server) echo "dbus,timers,cgroups" ;;
	full | all) echo "dbus,notify,cgroups,timers,sockets" ;;
	*)
		echo "unknown profile: $1 (try --list-profiles)" >&2
		return 1
		;;
	esac
}

choice_to_tags() {
	local c="$1"
	local t=""
	[[ "$c" == *"1"* ]] && t+="dbus,"
	[[ "$c" == *"2"* ]] && t+="sockets,"
	[[ "$c" == *"3"* ]] && t+="timers,"
	[[ "$c" == *"4"* ]] && t+="notify,"
	[[ "$c" == *"5"* ]] && t+="cgroups,"
	echo "$t" | sed 's/,$//'
}

resolve_tags() {
	local tags_out=""
	# CLI flags win over environment variables.
	if [[ -n "${CLI_TAGS:-}" ]]; then
		tags_out="${CLI_TAGS%,}"
	elif [[ -n "${SINS_TAGS:-}" ]]; then
		tags_out="${SINS_TAGS%,}"
	elif [[ -n "${SINS_CHOICE:-}" ]]; then
		if [[ "$SINS_CHOICE" == "0" ]]; then
			tags_out="dbus,notify,cgroups,timers,sockets"
		else
			tags_out="$(choice_to_tags "$SINS_CHOICE")"
		fi
	elif [[ -n "${CLI_PROFILE:-}" ]]; then
		tags_out="$(profile_to_tags "$CLI_PROFILE")" || return 1
	elif [[ -n "${SINS_PROFILE:-}" ]]; then
		tags_out="$(profile_to_tags "$SINS_PROFILE")" || return 1
	else
		tags_out="$(profile_to_tags minimal)"
	fi
	echo "$tags_out"
}

maybe_strip() {
	[[ "${SINS_STRIP:-}" == "1" ]] || return 0
	command -v strip >/dev/null 2>&1 || {
		echo "SINS_STRIP=1 set but strip not found; skip."
		return 0
	}
	echo "Stripping binaries (SINS_STRIP=1)..."
	for f in build/systemctl build/sins-daemon build/sins-timers build/sins-sockets build/libsystemd.so.0; do
		[[ -f "$f" ]] || continue
		strip "$f" 2>/dev/null || true
	done
}

# ----- argument parsing -----
CLI_PROFILE=""
CLI_TAGS=""
PRINT_TAGS=0
VERIFY=0

while [[ $# -gt 0 ]]; do
	case "$1" in
	--help | -h)
		usage
		exit 0
		;;
	--verify)
		VERIFY=1
		shift
		;;
	--list-profiles)
		list_profiles
		exit 0
		;;
	--print-tags)
		PRINT_TAGS=1
		shift
		;;
	--minimal)
		CLI_PROFILE=minimal
		shift
		;;
	--full | --all)
		CLI_PROFILE=full
		shift
		;;
	--profile)
		CLI_PROFILE="${2:?}"
		shift 2
		;;
	--tags)
		CLI_TAGS="${2:?}"
		shift 2
		;;
	--install)
		shift
		install_service
		exit 0
		;;
	*)
		echo "Unknown option: $1 (try --help)" >&2
		exit 2
		;;
	esac
done

if [[ "$VERIFY" == "1" ]]; then
	echo "Verifying Go packages (all modules)..."
	go build -tags "dbus,notify,cgroups,timers,sockets" -o /dev/null ./...
	go vet -tags "dbus,notify,cgroups,timers,sockets" ./...
	echo "Verifying Go packages (no optional tags)..."
	go build -o /dev/null ./...
	go vet ./...
	if can_build_stub; then
		echo "Verifying libsystemd stub (gcc)..."
		mkdir -p build
		gcc -shared -fPIC \
			-Wall -Wextra -Wno-unused-parameter \
			-pthread \
			-Wl,--version-script=pkg/libsystemd/libsystemd.map \
			-o build/libsystemd.so.0 pkg/libsystemd/stub.c pkg/libsystemd/journal.c -ldl
	else
		echo "Skipping stub verification (requires Linux x86_64)."
	fi
	echo "Verification SUCCESS."
	exit 0
fi

if [[ "$PRINT_TAGS" == "1" ]]; then
	tags="$(resolve_tags)" || exit 1
	echo "$tags"
	exit 0
fi

# Interactive menu when no explicit config and TTY
if [[ -z "${SINS_TAGS:-}" && -z "${CLI_TAGS:-}" && -z "${SINS_CHOICE:-}" && -z "${CLI_PROFILE:-}" && -z "${SINS_PROFILE:-}" && -t 0 ]]; then
	echo "SINS — pick a build profile (Enter = minimal, smallest install)"
	list_profiles
	echo ""
	echo "  [m] minimal   [d] dbus only   [e] de (dbus+notify)   [s] server   [f] full"
	echo "  [c] custom    legacy numbers 0=all 1=dbus 2=sockets 3=timers 4=notify 5=cgroups"
	read -r -p "Choice [m]: " _pick
	_pick="${_pick:-m}"
	case "${_pick,,}" in
	m | minimal) CLI_PROFILE=minimal ;;
	d | dbus) CLI_PROFILE=dbus ;;
	e | de | desktop) CLI_PROFILE=de ;;
	s | server) CLI_PROFILE=server ;;
	f | full) CLI_PROFILE=full ;;
	0 | 1 | 2 | 3 | 4 | 5 | 1,* | *,1 | *,*) export SINS_CHOICE="$_pick" ;;
	c | custom)
		echo "Comma-separated: 1=dbus 2=sockets 3=timers 4=notify 5=cgroups, or 0=all"
		read -r -p "Your choice: " SINS_CHOICE
		export SINS_CHOICE
		;;
	*)
		CLI_PROFILE=minimal
		;;
	esac
elif [[ -z "${SINS_TAGS:-}" && -z "${CLI_TAGS:-}" && -z "${SINS_CHOICE:-}" && -z "${CLI_PROFILE:-}" && -z "${SINS_PROFILE:-}" ]]; then
	: "${SINS_PROFILE:=minimal}"
fi

if [[ "${SINS_SYNC_STUB:-}" == "1" ]] && can_build_stub; then
	python3 "$REPO_ROOT/scripts/sync_stub_jumps.py"
fi

tags="$(resolve_tags)" || exit 1
tags="${tags// /}"

echo "-----------------------------------------"
echo "Resolved Go build tags: ${tags:-<none>}"
echo "-----------------------------------------"

go_tags_args=()
if [[ -n "$tags" ]]; then
	go_tags_args=(-tags "$tags")
fi

echo "Building systemctl..."
go build "${go_tags_args[@]}" -o build/systemctl ./cmd/systemctl

# sins-daemon without dbus would claim bus names with empty handlers — only build with dbus.
if [[ "$tags" == *"dbus"* ]]; then
	echo "Building sins-daemon..."
	go build "${go_tags_args[@]}" -o build/sins-daemon ./cmd/sins-daemon
else
	rm -f build/sins-daemon
	echo "Skipping sins-daemon (add 'dbus' tag / profile dbus or de or server or full)."
fi

if [[ "$tags" == *"sockets"* ]]; then
	echo "Building sins-sockets..."
	go build "${go_tags_args[@]}" -o build/sins-sockets ./cmd/socket-activator
else
	rm -f build/sins-sockets
	echo "Skipping sins-sockets (no 'sockets' tag)."
fi

if [[ "$tags" == *"timers"* ]]; then
	echo "Building sins-timers..."
	go build "${go_tags_args[@]}" -o build/sins-timers ./cmd/timers
else
	rm -f build/sins-timers
	echo "Skipping sins-timers (no 'timers' tag)."
fi

echo "Building sins-journalctl..."
go build -o build/sins-journalctl ./cmd/journalctl

if can_build_stub; then
	build_stub
else
	echo "Skipping libsystemd stub: requires Linux x86_64 (this host: $(uname -s) $(uname -m))."
fi

maybe_strip

echo "-----------------------------------------"
echo "Success! Contents of build/:"
ls -lh build/ 2>/dev/null || true
