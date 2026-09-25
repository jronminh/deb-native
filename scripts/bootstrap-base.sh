#!/bin/sh
# Install the set of packages real Debian assumes is "always already
# there" (Priority: required/important, part of every base install) as
# ONE bootstrap transaction, instead of pulling them in piecemeal as
# other packages' dependencies -- found the hard way
# (docs/findings.md) that installing them one at
# a time across separate apt-install.sh calls causes real bugs: a
# package's control scripts only get the shebang-rewrite-to-wrapper
# treatment if dash+wrapper already exist AT PATCH TIME (a real
# chicken-and-egg case — dash's own dependencies are patched before dash
# itself is unpacked), and a package left "unpacked but not configured"
# by an earlier call never gets a second patching pass unless something
# explicitly re-checks it.
#
# This script accepts that ITS OWN first pass runs under the limits of
# that chicken-and-egg case (dash doesn't exist yet when patch-deb.sh
# first touches these scripts) — apt-install.sh's post-unpack safety net
# (patch-maintainer-scripts.sh, run again after dash is unpacked and
# grun-patched, within the SAME call) re-patches every script's shebang
# once the wrapper becomes available, and a second --configure -a pass
# picks up anything that only needed that.
#
# Usage: bootstrap-base.sh NEWPREFIX
set -eu
NEWPREFIX=${1:?usage: bootstrap-base.sh NEWPREFIX}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# base-files/base-passwd: the actual Priority:required packages that ship
# /etc/passwd, /etc/shells's own skeleton, etc. -- most of what's had to
# be worked around (missing var/lib, missing /etc/shells) is exactly what
# these are supposed to provide on a real system.
# dash/debianutils: the shell maintainer scripts run under, and its
# common helpers (update-shells, which, run-parts, ...).
# debconf/cdebconf: the configuration-prompt system a large share of
# real packages' postinst scripts assume is present and configured.
# ca-certificates/openssl: needed by nearly anything that touches the
# network for real; also already-verified working test cases.
# mawk: base-files itself Pre-Depends on "awk" -- found by testing
# (dpkg refuses to even unpack base-files without it registered as
# configured first, a strict Pre-Depends ordering, not a normal Depends).
"$HERE/apt-install.sh" "$NEWPREFIX" \
  mawk base-files base-passwd dash debianutils debconf cdebconf \
  openssl ca-certificates
