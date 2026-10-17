/* trace.h -- the applier: carries out what the policy says at a stop. */
#ifndef TRACE_H
#define TRACE_H
#include "mon.h"
#include "policy.h"

extern unsigned long trace_n_rewritten;
extern unsigned long trace_n_answered;   /* calls answered at entry, with no exit stop */

/* build the synthetic /proc files (the mount view) once, at boot */
void trace_synth_init(void);

/* the persona for this boot: 1 = root (fake), 0 = user (the real app uid).
 * It is global until a per-process switch exists (spec/identity.md). */
void trace_persona(int root);

/* at syscall entry; may change the tracee's argument registers */
void trace_enter(struct tracee *t, const struct row *rw);
/* at the SIGSYS stop of a syscall the platform's filter killed: set the registers */
void trace_denied(struct tracee *t);
/* at syscall exit, for a row with a result to rewrite; may change x0 and the buffer */
void trace_exit(struct tracee *t, const struct row *rw);

/* /proc/<pid>/exe: the program a process runs, as the program should see it.
 * The exec gate notes it at entry, the exec event commits it, a fork copies it. */
void trace_exe_pending(pid_t pid, const char *guest_path);
void trace_exe_commit(pid_t pid);
void trace_exe_fork(pid_t parent, pid_t child);

/* at every stop of a process (a call with an effect is over by its next stop, so
 * the copies it may have changed are dropped again there), and when it is gone */
void trace_stop(pid_t pid);
void trace_gone(pid_t pid);

#endif
