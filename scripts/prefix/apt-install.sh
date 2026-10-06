#!/bin/sh
# Install packages (and their dependencies) into a prefix set up by
# setup-apt-prefix.sh. Since 0.2.0 this is plain apt: the prefix's apt.conf
# runs the translation pipeline in apt's own hooks (dn-hook-pre.sh /
# dn-hook-post.sh), so dpkg keeps its own Pre-Depends ordering and the
# 0.1.x one-package-at-a-time loop is gone (docs/spec/design.md).
#
# Usage: apt-install.sh PREFIX package [package...]
set -eu
DN=${1:?usage: apt-install.sh PREFIX package...}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
shift
TP=${DN_HOST_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
# Absolute path: a bare apt-get can resolve to a prefix's routing wrapper.
exec env APT_CONFIG="$DN/etc/apt.conf" "$TP/bin/apt-get" install -y "$@"
