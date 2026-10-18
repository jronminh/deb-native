/* trace.h -- the applier: carries out what the policy says at a stop. */
#ifndef TRACE_H
#define TRACE_H
#include "mon.h"
#include "policy.h"

extern unsigned long trace_n_rewritten;
extern unsigned long trace_n_answered;   /* calls answered at entry, with no exit stop */

/* build the synthetic /proc files (the mount view) once, at boot */
void trace_synth_init(void);

/* The identity a boot starts from: the app's real uid (the default) or 0 for a
 * root test session.  Behaviour is classified per syscall from the process's
 * current euid, not from this seed (spec/identity.md). */
void trace_identity_init(unsigned uid, unsigned gid);
/* the euid a setuid-bit exec will commit, applied on the exec event (exec.c) */
void trace_euid_next(pid_t pid, unsigned euid);
/* the process's current fake ids, for exec.c's DN_IDS hint */
void trace_ids_of(pid_t pid, unsigned out[6]);
/* the stored owner of a host path, 0 when there is none (exec.c's setuid gate) */
int trace_stored_owner(const char *host, unsigned *u, unsigned *g);

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
