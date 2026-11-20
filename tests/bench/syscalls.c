/* The syscalls Android 16 kills that have no row: open_tree, move_mount,
 * fsopen/fsconfig/fsmount/fspick, mount_setattr, fanotify_mark, setxattrat and
 * its family, file_getattr/file_setattr.  A killed number reaches the tracer as
 * SIGSYS and takes policy_deny's default (ENOSYS), so reaching the end of this
 * list is the test: without the answer the process would die at the first one.
 * Run inside a prefix (tests/bench/run.sh check). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static const long nr[] = { 428, 429, 430, 431, 432, 433, 442, 363,
			   463, 464, 465, 466, 468, 469, 0 };

int main(void)
{
	int i;

	for (i = 0; nr[i]; i++) {
		errno = 0;
		long r = syscall(nr[i], 0, 0, 0, 0, 0, 0);

		printf("%ld: %s%s\n", nr[i], r < 0 ? strerror(errno) : "ok",
		       r < 0 && errno == ENOSYS ? " (the default answer)" : "");
	}
	printf("survived %d killed numbers\n", i);
	return 0;
}
