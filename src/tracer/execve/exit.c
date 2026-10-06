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

#include <talloc.h>     /* talloc*, */

#include "execve/execve.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"

/**
 * Commit the new "/proc/self/exe" once the kernel has executed the
 * program (see translate_execve_enter()).  This function returns no
 * error: the calling process is already replaced, or the kernel's
 * error is propagated as-is.
 */
void translate_execve_exit(Tracee *tracee)
{
	tracee->auxv_fd = -1;
	tracee->restore_original_regs = false;

	if ((int) peek_reg(tracee, CURRENT, SYSARG_RESULT) < 0)
		return;

	/* The guest program is now running; PR_SET_NO_NEW_PRIVS calls from
	 * here on belong to the guest, not to PRoot's own pre-execve setup. */
	tracee->seen_execve = true;

	if (tracee->new_exe != NULL) {
		(void) talloc_unlink(tracee, tracee->exe);
		tracee->exe = talloc_reference(tracee, tracee->new_exe);
		talloc_set_name_const(tracee->exe, "$exe");
	}
}
