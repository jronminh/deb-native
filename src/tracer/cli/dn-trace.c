/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * dn-trace: deb-native's front end for the fork-lite tracer, replacing
 * PRoot's cli/cli.c + cli/proot.c (option tables, usage, extensions'
 * options, qemu, -r/-w/-0/...).  It keeps only what dn-run passes:
 *
 *   dn-trace [-v LEVEL] [-b HOST[:GUEST]]... [--] PROGRAM [ARG...]
 *
 * The guest root is always the host "/", the working directory is the
 * current one, and a -b whose host path does not exist is skipped
 * (PRoot warned about it), so dn-run need not check each prefix dir.
 * (A subset of proot's arguments; since 0.2.3 there is no proot fallback.)
 *
 * Derived from PRoot's cli/cli.c, Copyright (C) 2015 STMicroelectronics,
 * GPL-2.0-or-later like the rest of tracer/.
 */

#include <stdio.h>         /* fprintf(3), */
#include <stdbool.h>       /* bool, true, false, */
#include <linux/limits.h>  /* PATH_MAX, */
#include <string.h>        /* str*(3), */
#include <talloc.h>        /* talloc*, */
#include <stdlib.h>        /* exit(3), strtol(3), {g,s}etenv(3), */
#include <unistd.h>        /* getpid(2), */

#include "cli/note.h"
#include "tracee/tracee.h"
#include "tracee/event.h"
#include "path/binding.h"
#include "path/canon.h"
#include "path/path.h"

#define USAGE "usage: dn-trace [-v LEVEL] [-b HOST[:GUEST]]... [--] PROGRAM [ARG...]\n"

/* -b HOST[:GUEST]; GUEST defaults to HOST.  */
static int add_binding(Tracee *tracee, const char *value)
{
	char *host;
	char *guest;

	host = talloc_strdup(tracee->ctx, value);
	if (host == NULL)
		return -1;

	guest = strchr(host, ':');
	if (guest != NULL)
		*guest++ = '\0';

	/* must_exist = false: a missing host path is skipped quietly.  */
	(void) new_binding(tracee, host, guest, false);
	return 0;
}

/* Canonicalize the current working directory from the guest's view
 * (cli.c's initialize_cwd with cwd = ".").  */
static int initialize_cwd(Tracee *tracee)
{
	char path2[PATH_MAX];
	char path[PATH_MAX];
	int status;

	status = getcwd2(tracee->reconf.tracee, path);
	if (status < 0)
		return status;

	/* The ending "." makes canonicalize() fail on a non-directory.  */
	status = join_paths(3, path2, path, ".", ".");
	if (status < 0)
		return status;

	strcpy(path, "/");
	status = canonicalize(tracee, path2, true, path, 0);
	if (status < 0) {
		note(tracee, WARNING, USER, "can't chdir(\"%s\") in the guest: %s",
			path2, strerror(-status));
		strcpy(path, "/");
	}
	chop_finality(path);

	tracee->fs->cwd = talloc_strdup(tracee->fs, path);
	if (tracee->fs->cwd == NULL)
		return -1;
	talloc_set_name_const(tracee->fs->cwd, "$cwd");

	setenv("PWD", path, 1);
	return 0;
}

/* Resolve PROGRAM through PATH, as the guest sees it.  */
static int initialize_exe(Tracee *tracee, const char *exe)
{
	char path[PATH_MAX];
	int status;

	status = which(tracee, tracee->reconf.paths, path, exe);
	if (status < 0)
		return status;

	status = detranslate_path(tracee, path, NULL);
	if (status < 0)
		return status;

	tracee->exe = talloc_strdup(tracee, path);
	if (tracee->exe == NULL)
		return -1;
	talloc_set_name_const(tracee->exe, "$exe");
	return 0;
}

int main(int argc, char *const argv[])
{
	const char *verbose;
	Tracee *tracee;
	int status;
	int i;

	global_tool_name = "dn-trace";

	/* The first tracee (pid == 0) holds the configuration.  */
	tracee = get_tracee(NULL, 0, true);
	if (tracee == NULL)
		return EXIT_FAILURE;
	tracee->pid = getpid();
	tracee->tool_name = global_tool_name;

	verbose = getenv("PROOT_VERBOSE");
	if (verbose != NULL)
		tracee->verbose = strtol(verbose, NULL, 10);

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (argv[i][0] != '-')
			break;
		if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
			status = add_binding(tracee, argv[++i]);
		else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
			tracee->verbose = strtol(argv[++i], NULL, 10);
			status = 0;
		}
		else {
			fprintf(stderr, "dn-trace: unknown option '%s'\n" USAGE, argv[i]);
			return EXIT_FAILURE;
		}
		if (status < 0)
			goto error;
	}
	if (i >= argc) {
		fputs(USAGE, stderr);
		return EXIT_FAILURE;
	}
	global_verbose_level = tracee->verbose;

	/* The guest root is the host root; -b entries sit on top.  */
	if (new_binding(tracee, "/", "/", true) == NULL)
		goto error;

	status = initialize_bindings(tracee);
	if (status < 0)
		goto error;

	status = initialize_cwd(tracee);
	if (status < 0)
		goto error;

	/* which() reports a missing PROGRAM itself.  */
	status = initialize_exe(tracee, argv[i]);
	if (status < 0)
		goto error;

	status = launch_process(tracee, &argv[i]);
	if (status < 0) {
		note(tracee, ERROR, SYSTEM, "execve(\"%s\")", tracee->exe);
		goto error;
	}

	exit(event_loop());

error:
	TALLOC_FREE(tracee);
	return EXIT_FAILURE;
}
