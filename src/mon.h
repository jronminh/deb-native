/* mon.h -- what the catcher offers the applier: registers and memory of the
 * stopped tracee, and nothing about any syscall. */
#ifndef MON_H
#define MON_H

/* The per-process state (apply.h) is keyed by the full pid in a hash: there is
 * no fixed slot table and no pid_max assumption. */
#include "sys.h"

struct tracee {
	pid_t pid;
	struct user_pt_regs r;          /* fetched at the stop */
	int dirty;                      /* registers changed: the monitor writes them back */
	u64 scratch;                    /* the next free byte below the stack pointer, growing down */
	int skip;                       /* do not run the syscall: its result is in x0 */
	int mark_exit;                  /* this call may create a file: stop at exit and mark it */
	int dac_exit;                   /* a temporary owner-write bit to restore at exit */
	unsigned dac_mode;              /* the mode before the fake-root open */
	u64 entry_arg0;                 /* x0 as the syscall was entered; at exit x0 holds the result */
};

/* raw bytes at `addr`; bytes read (possibly fewer than `n`), or <= 0 */
ssize_t mon_read(struct tracee *t, u64 addr, void *buf, size_t n);
/* a NUL-terminated string at `addr`; bytes read, or <= 0 */
ssize_t mon_read_str(struct tracee *t, u64 addr, char *buf, size_t n);
/* put `n` bytes below the tracee's stack pointer; its address, or 0 */
u64 mon_scratch(struct tracee *t, const void *data, size_t n);
/* write `n` bytes at `addr` in the tracee; the bytes written */
ssize_t mon_write(struct tracee *t, u64 addr, const void *data, size_t n);
/* do not run this syscall; the program sees `value` as its result */
void mon_skip(struct tracee *t, i64 value);
/* change syscall argument register `i` */
void mon_set_arg(struct tracee *t, int i, u64 v);
/* run another syscall in place of the one being entered */
void mon_set_nr(struct tracee *t, int nr);

#endif
