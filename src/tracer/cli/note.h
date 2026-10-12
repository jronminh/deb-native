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

/* deb-native: the tree's glibc loader (ld.so), a host path derived
   from the tree: <TREE>/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1.
   Kept for the CLI contract; the exec gate's rule 3 no longer names it. */
extern const char *global_glibc_loader;

/* deb-native: the program interpreter (native/ld-dn.c), <TREE>/usr/lib/deb-native/
   ld-dn.  The exec gate runs a rule-3 (glibc dynamic) program through it
   (docs/spec/ld-dn-shim.md). */
extern const char *global_ld_dn;

/* deb-native: the tree (TREE), an absolute host path: the guest root, and
   home of the tracer's own temp dir (<TREE>/tmp; the host's /tmp is not
   writable on Android). */
extern const char *global_tree;

/* deb-native, -u: run with the real ids, fake root off (syscall/exit.c,
   dn_fake_root()). */
extern bool global_real_ids;

#endif /* NOTE_H */
