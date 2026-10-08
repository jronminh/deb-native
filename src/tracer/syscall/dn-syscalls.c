/* deb-native: the syscall-catalog reader.  See syscall/dn-syscalls.h. */
#include <string.h>		/* strcmp(3), strtok_r(3), strchr(3), */
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

int dn_catalog_gate_sysnums(TALLOC_CTX *context, Sysnum **out, size_t *count)
{
	const char *cursor = dn_catalog_tsv;
	char line[512];
	Sysnum *list = NULL;
	size_t n = 0;

	*out = NULL;
	*count = 0;

	while (*cursor != '\0') {
		char *name, *group, *handling, *glibc, *gate, *save;
		const char *end = strchr(cursor, '\n');
		size_t length = end != NULL ? (size_t) (end - cursor) : strlen(cursor);
		Sysnum sysnum;

		if (length >= sizeof line)
			return -EINVAL;
		memcpy(line, cursor, length);
		line[length] = '\0';
		cursor += length + (end != NULL);

		/* '#' comments (the provenance block, the column header) and
		 * blank lines are skipped.  */
		if (line[0] == '#' || line[0] == '\0')
			continue;

		name     = strtok_r(line, "\t", &save);
		group    = strtok_r(NULL, "\t", &save);
		handling = strtok_r(NULL, "\t", &save);
		glibc    = strtok_r(NULL, "\t", &save);
		gate     = strtok_r(NULL, "\t", &save);
		(void) group; (void) handling; (void) glibc;

		if (name == NULL || gate == NULL || strcmp(gate, "yes") != 0)
			continue;

		sysnum = dn_name_to_sysnum(name);
		if (sysnum == PR_void)
			continue;

		list = talloc_realloc(context, list, Sysnum, n + 1);
		if (list == NULL)
			return -ENOMEM;
		list[n++] = sysnum;
	}

	*out = list;
	*count = n;
	return 0;
}
