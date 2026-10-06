#!/bin/sh
# Orchestrate a prefix's runtime: build the artifacts (build stage), install
# them and the privilege layer (core). This is the bootstrap-level glue, so
# `core` itself never reaches build or adapter (MODULARIZE.md P2). Idempotent.
#
# The interpreter is a real ELF built from native/dn-launch.c; it needs a
# glibc bash to exec. Reason for the real ELF (not a shell script):
# docs/log/findings/complete-base-bootstrap.md -- the kernel follows only one
# `#!` level, and a script interpreter leaves dpkg falling back to Bionic
# /bin/sh with no shim.
#
# Usage: setup-runtime.sh INSTDIR
set -eu
INSTDIR=${1:?usage: setup-runtime.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
GLIBC=${DN_GLIBC_ROOT:-$PREFIX_DIR/glibc}

[ -x "$GLIBC/bin/bash" ] || { echo "E: no glibc bash at $GLIBC/bin/bash (pkg install bash-glibc)" >&2; exit 1; }

"$HERE/../build/build-core.sh"
"$HERE/../core/install/install-runtime.sh" "$INSTDIR"
