/* exec.h -- the exec gate, a part of the applier. */
#ifndef EXEC_H
#define EXEC_H
#include "mon.h"

extern unsigned long exec_n_rewritten;

/* at execve entry: map the path, resolve scripts and glibc ELFs, rewrite argv */
void exec_enter(struct tracee *t);

#endif
