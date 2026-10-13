/* policy.h -- what the catcher and the applier are told; the only place that
 * knows which syscall means what.  In the real tree this is dn-policy. */
#ifndef POLICY_H
#define POLICY_H
#include <stddef.h>
#include "sys.h"

/* One row per syscall to catch.  The monitor builds its filter from the rows
 * and knows nothing else about any syscall. */
struct row {
	unsigned nr;          /* arm64 syscall number */
	unsigned exit_stop;   /* 1: stop again at exit (the result is needed) */
	int path_arg;         /* register index of a path argument, or -1 */
	int path_arg2;        /* a second path argument (rename, link), or -1 */
	int special;          /* SP_*: handled by a dedicated part of the applier */
	int fake;             /* F_*: answer at entry without running the syscall (fake root) */
	int result;           /* R_*: rewrite what the syscall returned, at exit */
	int result_arg;       /* register index of the buffer the result is in (R_STAT, R_STATX) */
};

/* R_CWD: getcwd's buffer.  R_LINK: readlinkat's buffer, and /proc/<pid>/exe.
 * R_STAT, R_STATX: the owner in a stat buffer. */
enum { R_NONE = 0, R_CWD = 1, R_LINK = 2, R_STAT = 3, R_STATX = 4 };

enum { SP_NONE = 0, SP_EXEC = 1, SP_LINK = 2 };
/* F_ZERO: return 0.  F_RESIDS: getresuid/getresgid, write three zero ids.
 * F_GROUPS: getgroups, one group, 0. */
enum { F_NONE = 0, F_ZERO = 1, F_RESIDS = 2, F_GROUPS = 3, F_EXDEV = 4, F_GETID = 5, F_SETID = 6 };
/* F_GETID, F_SETID: the ids the process was last given (fake root keeps them per process). */

size_t policy_count(void);
const struct row *policy_at(size_t i);
const struct row *policy_row(unsigned nr);     /* NULL: not caught */

void policy_set_root(const char *root);

/* The law for a path: 0 if `in` stays as it is, else the length of the
 * rewritten path (NUL included) written to `out`. */
int policy_path(const char *in, char *out, size_t cap);
/* The same with symlinks resolved against the prefix (an absolute link target is
 * the prefix's, not the host's); `nofollow`: leave the last component as a link. */
int policy_path_nf(const char *in, char *out, size_t cap, int nofollow);
/* Does the syscall act on the link itself, for the path in register `arg`? */
int policy_nofollow(unsigned nr, int arg, const u64 *regs);
/* A relative path from the host directory `base`; the register of its directory fd. */
int policy_path_at(const char *base, const char *in, char *out, size_t cap, int nofollow);
int policy_dirfd_arg(unsigned nr, int arg);

/* The reverse of policy_path, for what the kernel returns: a host path as the
 * program should see it.  0 if it stays, else the length (NUL included) written
 * to `out`. */
int policy_unpath(const char *host, char *out, size_t cap);

/* Fake root's owner: the real uid and gid of this process read as root. */
void policy_set_ids(unsigned uid, unsigned gid);
unsigned policy_map_uid(unsigned id);
unsigned policy_map_gid(unsigned id);

/* How a glibc program is started: the loader to run it through and the
 * library search path to give it, both as host paths. */
const char *policy_loader(void);
const char *policy_libpath(void);
/* Is this PT_INTERP the Debian glibc loader (a path that does not exist here)? */
int policy_is_glibc_interp(const char *interp);

/* The answer to a syscall the platform's own filter killed (it arrives as
 * SIGSYS).  D_RESULT: put `value` in the result register (0 or -errno).
 * D_SUBST: run the equivalent syscall `to` instead, with the arguments as
 * they are (the extra ones are ignored); if `flags_arg` is set and carries a bit outside `ok_mask`, the
 * substitute cannot honour it and the answer is ENOSYS instead (the program's
 * own fallback then does it).  D_OPENAT2: openat2 as openat, from its open_how. */
enum { D_RESULT = 0, D_SUBST = 1, D_OPENAT2 = 2 };
struct deny {
	int kind;
	i64 value;
	unsigned to;
	int flags_arg;        /* register index of the flags argument, or -1 */
	u64 ok_mask;          /* the flag bits the substitute honours */
	int zero_from;        /* D_SUBST: clear the arguments from this register on (an extra one is 0), or -1 */
};
void policy_deny(unsigned nr, struct deny *d);

#endif
