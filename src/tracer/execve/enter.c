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

#include <sys/types.h>  /* lstat(2), */
#include <sys/stat.h>   /* lstat(2), */
#include <unistd.h>     /* access(2), lstat(2), */
#include <errno.h>      /* E*, */
#include <talloc.h>     /* talloc*, */
#include <string.h>     /* strcpy(3), */

#include "execve/execve.h"
#include "execve/shebang.h"
#include "execve/elf.h"
#include "path/path.h"
#include "tracee/tracee.h"
#include "syscall/syscall.h"
#include "cli/note.h"

/**
 * Translate @user_path into @host_path and check if this latter exists, is
 * executable and is a regular file.  This function returns -errno if
 * an error occured, 0 otherwise.
 */
int translate_and_check_exec(Tracee *tracee, char host_path[PATH_MAX], const char *user_path)
{
	struct stat statl;
	int status;

	if (user_path[0] == '\0')
		return -ENOEXEC;

	status = translate_path(tracee, host_path, AT_FDCWD, user_path, true);
	if (status < 0)
		return status;

	status = access(host_path, F_OK);
	if (status < 0)
		return -ENOENT;

	status = access(host_path, X_OK);
	if (status < 0)
		return -EACCES;

	status = lstat(host_path, &statl);
	if (status < 0)
		return -EPERM;

	return 0;
}

/* deb-native, P1 (docs/spec/runtime.md, "The exec gate"): classify the
 * final @host_path by the 5 rules there, purely from its header -- no
 * trial run. Observe-only for now: the result is only logged, nothing
 * here changes what gets exec'd. P2 wires this into dn-policy and
 * rewrites the exec per rules 3/4.  */

#define DN_GLIBC_LOADER_SUFFIX "/ld-linux-aarch64.so.1"

typedef struct {
	int fd;
	char interp[PATH_MAX];
	bool found;
} FindInterp;

/* iterate_program_headers() callback: copy out PT_INTERP's string, then
 * stop iterating (return 1).  */
static int find_interp(const ElfHeader *elf_header, const ProgramHeader *program_header, void *data)
{
	FindInterp *result = data;
	uint64_t offset;
	uint64_t size;

	if (PROGRAM_FIELD(*elf_header, *program_header, type) != PT_INTERP)
		return 0;

	offset = PROGRAM_FIELD(*elf_header, *program_header, offset);
	size   = PROGRAM_FIELD(*elf_header, *program_header, filesz);
	if (size == 0 || size >= sizeof(result->interp))
		return -ENOTSUP;

	if (lseek(result->fd, offset, SEEK_SET) < 0)
		return -errno;
	if (read(result->fd, result->interp, size) != (ssize_t) size)
		return -EIO;

	result->interp[size] = '\0';
	result->found = true;
	return 1;
}

/* Rules 2-5. Rule 1 (the "#!" script case) is already unwrapped by
 * expand_shebang() before this runs, so @host_path here is always the
 * final ELF (or the final non-ELF, for rule 5).  */
static const char *classify_exec(const char *host_path)
{
	ElfHeader elf_header;
	FindInterp result = { .found = false };
	size_t suffix_len = sizeof(DN_GLIBC_LOADER_SUFFIX) - 1;
	size_t interp_len;
	int status;

	result.fd = open_elf(host_path, &elf_header);
	if (result.fd < 0)
		return "rule5-not-runnable";

	status = iterate_program_headers(NULL, result.fd, &elf_header, find_interp, &result);
	close(result.fd);

	if (status < 0 && status != 1)
		return "elf-read-error";

	if (!result.found)
		return "rule2-static";

	interp_len = strlen(result.interp);
	if (interp_len >= suffix_len
	    && strcmp(result.interp + interp_len - suffix_len, DN_GLIBC_LOADER_SUFFIX) == 0)
		return "rule3-glibc-dynamic";

	return "rule4-foreign";
}

/**
 * deb-native: the kernel execs the translated program itself.
 *
 * PRoot runs its own loader instead of the program and has it map the
 * program and its ELF interpreter, so that a guest-rootfs PT_INTERP
 * can be found.  In a deb-native prefix every interpreter is already a
 * host path (the prefix's own glibc loader, Termux's glibc loader, Bionic's linker64) and
 * static programs have none, so the kernel can load them directly:
 * only the program path (and a script's "#!" interpreter, see
 * expand_shebang()) needs translating.  The loader, its load script,
 * PRoot's brk emulation and the qemu runner are therefore gone.
 *
 * This function returns -errno if an error occured, otherwise 0.
 */
int translate_execve_enter(Tracee *tracee)
{
	char user_path[PATH_MAX];
	char host_path[PATH_MAX];
	char new_exe[PATH_MAX];
	int status;

	status = get_sysarg_path(tracee, user_path, SYSARG_1);
	if (status < 0)
		return status;

	status = expand_shebang(tracee, host_path, user_path);
	if (status < 0)
		/* The Linux kernel actually returns -EACCES when
		 * trying to execute a directory.  */
		return status == -EISDIR ? -EACCES : status;

	/* P1 exec gate: classify and log only, see classify_exec() above.  */
	VERBOSE(tracee, 1, "exec gate: %s -> %s", host_path, classify_exec(host_path));

	/* Remember the new value for "/proc/self/exe", committed by
	 * translate_execve_exit() once the execve succeeded.  It is
	 * a guest path, hence detranslate_path().  */
	talloc_unlink(tracee, tracee->host_exe);
	tracee->host_exe = talloc_strdup(tracee, host_path);

	talloc_unlink(tracee, tracee->new_exe);
	tracee->new_exe = NULL;
	strcpy(new_exe, host_path);
	status = detranslate_path(tracee, new_exe, NULL);
	if (status >= 0)
		tracee->new_exe = talloc_strdup(tracee, new_exe);

	return set_sysarg_path(tracee, host_path, SYSARG_1);
}
