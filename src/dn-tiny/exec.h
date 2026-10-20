/* exec.h -- the exec gate, a part of the applier. */
#ifndef EXEC_H
#define EXEC_H
#include "mon.h"

extern unsigned long exec_n_rewritten;

/* run glibc programs through dn-exec (usr/lib/deb-native/dn-exec) instead of
 * the loader; the A/B switch for the revision. */
void exec_use_dnexec_set(int v);

/* at execve entry: map the path, resolve scripts and glibc ELFs, rewrite argv */
void exec_enter(struct tracee *t);

#endif
