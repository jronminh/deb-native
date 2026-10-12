/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2015 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#ifndef DN_MONITOR_H
#define DN_MONITOR_H

#include "syscall/sysnum.h"
#include "tracee/tracee.h"
#include "attribute.h"
#include "arch.h"

/* dn-monitor -- deb-native's catch/route mechanism (docs/spec/monitor.md).
 *
 * It owns the seccomp filter that catches syscalls and the two-tap routing:
 *
 *   1. the filter (static, built here from the published catalog,
 *      src/syscalls.tsv, embedded at build time).  In the kernel it cannot
 *      ask dn-policy at run time, so dn-policy's classification is
 *      *materialised into the filter* when the filter is built.  It returns:
 *        - from the gate page      -> ALLOW (dn-shim already did it);
 *        - a handled group, not
 *          from the gate           -> TRACE;
 *        - otherwise               -> ALLOW.
 *
 *   2. the dispatcher (runtime, per dn-policy): a TRACEd call is routed by
 *      what dn-policy says the platform provides -- provided -> dn-trace
 *      (apply the mapping law); refused / divergent -> dn-emu (emulate it,
 *      or refuse clearly).  That tap lives with the event loop; this unit is
 *      tap 1 and the flags it publishes.
 *
 * dn-shim front-runs the monitor (in-process, before the filter); the monitor
 * never dispatches to it, it only acknowledges it through the gate mark above.
 */

typedef struct {
	Sysnum value;
	word_t flags;
} FilteredSysnum;

typedef struct {
	unsigned int value;
	size_t nb_abis;
	Abi abis[NB_MAX_ABIS];
} SeccompArch;

#define FILTERED_SYSNUM_END { PR_void, 0 }

#define FILTER_SYSEXIT  0x1

/* Install the filter -- the catch/route tap 1 -- for @tracee and every child
 * it creates.  Returns -errno on error, otherwise 0. */
extern int dn_monitor_install(const Tracee *tracee);

/* The flags the filter installed by dn_monitor_install() associates with
 * @sysnum: the data a PTRACE_EVENT_SECCOMP stop of that syscall reports (the
 * filter was built from the very same lists). */
extern int dn_monitor_sysnum_flags(const Tracee *tracee, Sysnum sysnum);

#endif /* DN_MONITOR_H */
