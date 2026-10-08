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

#ifndef NOTE_H
#define NOTE_H

#include "tracee/tracee.h"
#include "attribute.h"

/* Specify where a notice is coming from. */
typedef enum {
	SYSTEM,
	INTERNAL,
	USER,
	TALLOC,
} Origin;

/* Specify the severity of a notice. */
typedef enum {
	ERROR,
	WARNING,
	INFO,
} Severity;

#define VERBOSE(tracee, level, message, args...) do {			\
		if (tracee == NULL || tracee->verbose >= (level))	\
			note(tracee, INFO, INTERNAL, (message), ## args); \
	} while (0)

extern void note(const Tracee *tracee, Severity severity, Origin origin, const char *message, ...) FORMAT(printf, 4, 5);

extern int global_verbose_level;
extern const char *global_tool_name;

/* deb-native: the runtime's loader (dn-glibc's ld.so), from --rt-loader.
   When set, the exec gate runs a rule-3 (glibc dynamic) program through it
   instead of exec'ing the program directly -- the tree's own loader has no
   dn-policy wiring, this one does (docs/spec/runtime.md, "The exec gate").
   NULL by default: behavior is unchanged. */
extern const char *global_rt_loader;

/* deb-native: the published syscall catalog (src/syscalls.tsv), from
   --syscalls.  When set, the seccomp filter's gate-IP exemption is built
   from it (syscall/dn-syscalls.c).  NULL = no exemption. */
extern const char *global_syscalls_path;

#endif /* NOTE_H */
