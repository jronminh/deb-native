/* deb-native: the syscall-catalog reader.  See syscall/dn-syscalls.h. */
#include <stdio.h>		/* fopen(3), fgets(3), */
#include <string.h>		/* strcmp(3), strtok_r(3), */
#include <errno.h>		/* E*, */

#include "syscall/dn-syscalls.h"

/* Map a catalog name ("openat") to its Sysnum without a hand table:
 * stringify_sysnum() returns exactly those names (sysnums.list).  */
static Sysnum dn_name_to_sysnum(const char *name)
{
	Sysnum sysnum;

	for (sysnum = 1; sysnum < PR_NB_SYSNUM; sysnum++) {
		const char *s = stringify_sysnum(sysnum);
		if (s != NULL && s[0] != '\0' && strcmp(s, name) == 0)
			return sysnum;
	}
	return PR_void;
}

int dn_catalog_gate_sysnums(TALLOC_CTX *context, const char *path,
			    Sysnum **out, size_t *count)
{
	char line[512];
	Sysnum *list = NULL;
	size_t n = 0;
	FILE *f;

	*out = NULL;
	*count = 0;

	if (path == NULL)
		return -ENOENT;

	f = fopen(path, "r");
	if (f == NULL)
		return -errno;

	while (fgets(line, sizeof line, f) != NULL) {
		char *name, *group, *handling, *glibc, *gate, *save;
		Sysnum sysnum;

		/* '#' comments (the provenance block, the column header) and
		 * blank lines are skipped.  */
		if (line[0] == '#' || line[0] == '\n')
			continue;

		name     = strtok_r(line, "\t\n", &save);
		group    = strtok_r(NULL, "\t\n", &save);
		handling = strtok_r(NULL, "\t\n", &save);
		glibc    = strtok_r(NULL, "\t\n", &save);
		gate     = strtok_r(NULL, "\t\n", &save);
		(void) group; (void) handling; (void) glibc;

		if (name == NULL || gate == NULL || strcmp(gate, "yes") != 0)
			continue;

		sysnum = dn_name_to_sysnum(name);
		if (sysnum == PR_void)
			continue;

		list = talloc_realloc(context, list, Sysnum, n + 1);
		if (list == NULL) {
			fclose(f);
			return -ENOMEM;
		}
		list[n++] = sysnum;
	}
	fclose(f);

	*out = list;
	*count = n;
	return 0;
}
