#!/bin/sh
# Install packages (and their dependencies) into a prefix. Since 0.2.0 this is
# plain apt: with runtime v1 a .deb installs intact (no translation hook), and
# dn-policy rewrites paths and identities at run time (docs/spec/runtime.md).
#
# Usage: apt-install.sh PREFIX package [package...]
set -eu
DN=${1:?usage: apt-install.sh PREFIX package...}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
shift
TP=${DN_HOST_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
# Absolute path: a bare apt-get can resolve to a prefix's routing wrapper.
exec env APT_CONFIG="$DN/etc/apt.conf" "$TP/bin/apt-get" install -y "$@"
