#!/usr/bin/env bash
# Quick checks that matter for AUR/libsystemd consumers.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -f build/libsystemd.so.0 ]]; then
  echo "smoke_aur: run ./build.sh --profile minimal first" >&2
  exit 2
fi

echo "smoke_aur: ldd libsystemd.so"
ldd build/libsystemd.so.0 | head -5

if command -v pkg-config >/dev/null 2>&1; then
  if pkg-config --exists libsystemd 2>/dev/null; then
    echo "smoke_aur: pkg-config libsystemd -> $(pkg-config --libs libsystemd)"
  else
    echo "smoke_aur: no system libsystemd.pc (common); AUR often passes -lsystemd anyway"
  fi
fi

echo "smoke_aur: OK"
#!/usr/bin/env bash
# Quick checks that matter for AUR / libsystemd consumers (no network).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ ! -f build/libsystemd.so.0 ]]; then
	echo "smoke_aur: run ./build.sh --profile minimal first" >&2
	exit 2
fi
echo "smoke_aur: ldd libsystemd.so"
ldd build/libsystemd.so.0 | head -5
if command -v pkg-config >/dev/null 2>&1; then
	if pkg-config --exists libsystemd 2>/dev/null; then
		echo "smoke_aur: pkg-config libsystemd -> $(pkg-config --libs libsystemd)"
	else
		echo "smoke_aur: no system libsystemd.pc (common); AUR often passes -lsystemd anyway"
	fi
fi
echo "smoke_aur: OK"
