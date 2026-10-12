#!/bin/sh
# The tree's boot (docs/spec/prefix.md, "Boot"): start dn-monitor, the tree's
# root process, against this prefix and its init.
#
# Why a script and not the bare command line.  dn-monitor is the *outermost*
# process -- nothing traces it -- so its own startup must survive Android's
# seccomp unaided.  A stock static glibc registers `rseq` at startup, and the
# app domain TRAPs it: SIGSYS, and with no tracer above to answer it, the
# process dies.  The supported tunable `glibc.pthread.rseq=0` turns that
# registration off, and glibc reads tunables before `main` -- so the variable
# has to be in the environment of the exec that starts dn-monitor, i.e. set by
# whoever launches it.  This script is that launcher, and it is the only place
# the boot's environment is decided.
#
# The contract's `entry` is this path; the host runs it with the prefix root as
# the working directory, directly (the shebang resolves on the host) or through
# a shell.  Any arguments are passed on to the prefix's init, which takes
# [COMMAND [ARG...]] (docs/spec/prefix.md).
GLIBC_TUNABLES=glibc.pthread.rseq=0
export GLIBC_TUNABLES

exec "${0%/*}/dn-monitor" -- /usr/bin/bash /usr/lib/deb-native/init.sh "$@"
