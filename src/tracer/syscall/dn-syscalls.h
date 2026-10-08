/* deb-native: read the published syscall catalog (src/syscalls.tsv) so
 * dn-trace builds its filter from the one list instead of hand-maintaining
 * its own (docs/spec/runtime.md, "The shared filter's rules"; principle 1).
 * The catalog is installed next to dn-trace in RT and named by --syscalls.
 */
#ifndef DN_SYSCALLS_H
#define DN_SYSCALLS_H

#include <talloc.h>		/* TALLOC_CTX, */
#include "syscall/sysnum.h"	/* Sysnum, */

/* Return, talloc'd on @context, the syscalls the filter may ALLOW when issued
 * from the gate page (catalog rows with gate=yes).  Returns 0, or -errno
 * (-ENOENT when @path is missing -- the caller then has no gate exemption,
 * which is safe).  */
int dn_catalog_gate_sysnums(TALLOC_CTX *context, const char *path,
			    Sysnum **out, size_t *count);

#endif /* DN_SYSCALLS_H */
