/* monitor.c -- the catcher: the ancestor of every process in the prefix.
 *
 *   dn-tiny [-m bare|regs|full] [-t ROOT] [-c CPU] [-s USEC] [-q] -- PROGRAM [ARG...]
 *
 * It installs one seccomp filter built from the policy's rows, traces the whole
 * tree through ptrace's automatic attach, and at each stop hands the stopped
 * tracee to the applier.  It knows nothing about any syscall.  aarch64 only,
 * and no libc: see sys.h.
 *
 * The mechanism is PRoot's (GPL-2.0-or-later); this is a new, much smaller
 * implementation of the same technique.  See CREDITS.md.
 *
 *   bare   restart only (the floor of a stop)
 *   regs   + fetch the registers
 *   full   + look the syscall up in the policy and apply it (default)
 *
 * -s USEC: before blocking for the next stop, poll for it for up to USEC
 * microseconds (off by default).  A tracer that sleeps between stops lets its core
 * drop to a low clock or an idle state that every stop then pays to leave; polling
 * keeps it awake during a burst of stops, at the price of the CPU time spent
 * polling.  Whether that pays is the host's decision, not the tracer's.
 */
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/ptrace.h>
#include <linux/seccomp.h>
#include "exec.h"
#include "mon.h"
#include "policy.h"
#include "trace.h"
#include "lane.h"

#define NT_PRSTATUS 1
#define NT_ARM_SYSTEM_CALL 0x404
#define SIGSTOP_ 19
#define SIGTRAP_ 5
#define SIGSYS_ 31
#define WALL 0x40000000
#define SYS_SECCOMP_CODE 1

#define WIFEXITED_(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS_(s) (((s) >> 8) & 0xff)
#define WIFSTOPPED_(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG_(s)    (((s) >> 8) & 0xff)
#define WTERMSIG_(s)    ((s) & 0x7f)

enum { M_BARE, M_REGS, M_FULL };
static int mode = M_FULL;
static i64 spin_ns;                              /* -s: poll this long before blocking, 0: never */
static unsigned long n_enter, n_exit, n_event, n_sig, n_sigsys;
#ifndef DN_HOST_ADB
static unsigned deny_count[512];                 /* by syscall number: killed by the platform, answered here */
#endif
#define EXP(p) (pid_state_get((p), 1)->expect_exit)

/* ---- the filter: one TRACE rule per row -------------------------------- */
/* The program is built with labels, so no jump offset is counted by hand: a
 * conditional jump names the label it goes to when true and when false, and
 * resolve() turns them into offsets.  One JGE tree holds the rows: a number no row
 * lists is one tree away from ALLOW, and a row the lane also lists tests the shim's
 * cookie (args[5]) in its own leaf.  Only those rows read an argument, which is what
 * keeps the kernel (5.11 and later) from caching its answer per number. */
#define FNINS 1024                     /* BPF_MAXINSNS is 4096; this program is about 400 */
#define FNLAB 512
struct fjump { size_t pc; int lt, lf; int kind; };   /* kind 0: conditional, 1: JA */
struct fprog {
	struct sock_filter f[FNINS];
	size_t n;
	size_t lab[FNLAB];
	int nlab;
	struct fjump fx[FNINS];
	size_t nfx;
	int bad;
};

static int flabel(struct fprog *p)
{
	if (p->nlab >= FNLAB) { p->bad = 1; return 0; }
	p->lab[p->nlab] = (size_t) -1;
	return p->nlab++;
}

static void fplace(struct fprog *p, int l)
{
	p->lab[l] = p->n;
}

static void fins(struct fprog *p, unsigned short code, unsigned char jt, unsigned char jf, unsigned k)
{
	if (p->n >= FNINS) { p->bad = 1; return; }
	p->f[p->n] = (struct sock_filter) { code, jt, jf, k };
	p->n++;
}

static void fstmt(struct fprog *p, unsigned short code, unsigned k)
{
	fins(p, code, 0, 0, k);
}

static void fjmp(struct fprog *p, unsigned short code, unsigned k, int lt, int lf)
{
	if (p->nfx >= FNINS) { p->bad = 1; return; }
	p->fx[p->nfx] = (struct fjump) { p->n, lt, lf, 0 };
	p->nfx++;
	fins(p, code, 0, 0, k);
}

static void fja(struct fprog *p, int l)
{
	if (p->nfx >= FNINS) { p->bad = 1; return; }
	p->fx[p->nfx] = (struct fjump) { p->n, l, 0, 1 };
	p->nfx++;
	fins(p, BPF_JMP | BPF_JA, 0, 0, 0);
}

/* offsets are relative to the next instruction; a conditional's offset must fit in
 * 8 bits, which the tree below keeps true for the policy's rows (checked here) */
static int fresolve(struct fprog *p)
{
	size_t i;

	for (i = 0; i < p->nfx; i++) {
		struct fjump *j = &p->fx[i];
		size_t tgt_t = p->lab[j->lt];

		if (tgt_t == (size_t) -1 || tgt_t <= j->pc) return -1;
		if (j->kind == 1) {
			p->f[j->pc].k = (unsigned) (tgt_t - j->pc - 1);
			continue;
		}
		{
			size_t tgt_f = p->lab[j->lf];
			size_t dt = tgt_t - j->pc - 1;

			if (tgt_f == (size_t) -1 || tgt_f <= j->pc) return -1;
			if (dt > 255 || tgt_f - j->pc - 1 > 255) return -1;
			p->f[j->pc].jt = (unsigned char) dt;
			p->f[j->pc].jf = (unsigned char) (tgt_f - j->pc - 1);
		}
	}
	return p->bad ? -1 : 0;
}

/* the rows, sorted by number, as a tree: a node compares the number with the first
 * number of its right half (JGE), and a leaf of at most two rows compares each */
static const struct row *rows_by_nr[512];

/* a row the shim also answers, and so must test the lane's cookie: 1 lane,
 * 2 openat (a write in its flags still reaches the tracer) */
static unsigned char lane_kind[512];

/* What a matched row does.  A row the lane does not list is one TRACE, right in
 * the leaf.  A row the lane lists branches (a JA, a far jump) to one shared block:
 * the cookie is tested once for every lane row, not once per leaf, so the filter
 * stays small.  Two plain blocks (only getcwd, readlinkat, newfstatat and statx
 * have a result to rewrite, so they stop again at exit and the others do not) and
 * one for openat, whose write flags must reach the tracer. */
struct lane_blk { int lc0, lc1, lop; };
static struct lane_blk lb;

static void emit_row(struct fprog *p, const struct row *rw)
{
	unsigned kind = rw->nr < 512 ? lane_kind[rw->nr] : 0;

	if (!kind) {
		fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_TRACE | (rw->exit_stop & SECCOMP_RET_DATA));
		return;
	}
	fja(p, kind == 2 ? lb.lop : (rw->exit_stop & SECCOMP_RET_DATA) ? lb.lc1 : lb.lc0);
}

/* the lane: the cookie in x5 (both halves); a match ALLOWs the call, else the row's
 * TRACE.  openat, too, only to read: a flag that writes falls back to the tracer. */
static void emit_cookie(struct fprog *p, int stop)
{
	int lc2 = flabel(p), lc3 = flabel(p), lf = flabel(p);

	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]));
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_LO, lc2, lf);
	fplace(p, lc2);
	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_HI, lc3, lf);
	fplace(p, lc3);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	fplace(p, lf);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_TRACE | (stop & SECCOMP_RET_DATA));
}

static void emit_openat(struct fprog *p)
{
	int lc2 = flabel(p), lc3 = flabel(p), lallow = flabel(p), lf = flabel(p);

	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]));
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_LO, lc2, lf);
	fplace(p, lc2);
	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_HI, lc3, lf);
	fplace(p, lc3);
	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2]));
	fjmp(p, BPF_JMP | BPF_JSET | BPF_K, DN_LANE_OPEN_DENY, lf, lallow);
	fplace(p, lallow);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	fplace(p, lf);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_TRACE);
}

static void frows(struct fprog *p, size_t lo, size_t hi)
{
	size_t cnt = hi - lo;

	if (cnt <= 2) {
		size_t i;

		for (i = lo; i < hi; i++) {
			const struct row *rw = rows_by_nr[i];
			int ltrue = flabel(p), lnext = flabel(p);

			fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, rw->nr, ltrue, lnext);
			fplace(p, ltrue);
			emit_row(p, rw);
			fplace(p, lnext);
		}
		fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
		return;
	}
	{
		size_t m = lo + cnt / 2;
		int lright = flabel(p), lleft = flabel(p);

		fjmp(p, BPF_JMP | BPF_JGE | BPF_K, rows_by_nr[m]->nr, lright, lleft);
		fplace(p, lleft);
		frows(p, lo, m);
		fplace(p, lright);
		frows(p, m, hi);
	}
}

static struct fprog fp;

static int install_filter(void)
{
	static const unsigned lane[] = DN_LANE_NRS;
	const size_t nl = sizeof lane / sizeof lane[0];
	struct sock_fprog prog;
	size_t i, j;
	int lok, lnotarch, lsock, lnl, lerr, lrows;

	/* the rows, sorted by number: a number in two rows would leave one unreachable */
	if (policy_count() > 512) return -1;
	for (i = 0; i < policy_count(); i++) rows_by_nr[i] = policy_at(i);
	for (i = 1; i < policy_count(); i++) {
		const struct row *x = rows_by_nr[i];
		j = i;
		while (j > 0 && rows_by_nr[j - 1]->nr > x->nr) {
			rows_by_nr[j] = rows_by_nr[j - 1];
			j--;
		}
		rows_by_nr[j] = x;
	}
	for (i = 1; i < policy_count(); i++)
		if (rows_by_nr[i]->nr == rows_by_nr[i - 1]->nr) return -1;

	/* which rows the shim also answers, and so must test the cookie */
	for (i = 0; i < 512; i++) lane_kind[i] = 0;
	for (i = 0; i < nl; i++)
		if (lane[i] < 512) lane_kind[lane[i]] = 1;
	if (DN_LANE_OPENAT < 512) lane_kind[DN_LANE_OPENAT] = 2;

	fp.n = fp.nlab = fp.nfx = 0;
	fp.bad = 0;

	/* the architecture: anything else is allowed as the kernel would do */
	lok = flabel(&fp); lnotarch = flabel(&fp);
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
	fjmp(&fp, BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, lok, lnotarch);
	fplace(&fp, lnotarch);
	fstmt(&fp, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	fplace(&fp, lok);

	/* the audit socket: Android's policy refuses it (EACCES), and libaudit gives up on
	 * that where it accepts a kernel without audit (shadow's useradd aborts).  The
	 * filter says what such a kernel says, with no stop. */
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	lsock = flabel(&fp); lrows = flabel(&fp);
	fjmp(&fp, BPF_JMP | BPF_JEQ | BPF_K, 198 /* socket */, lsock, lrows);
	fplace(&fp, lsock);
	lnl = flabel(&fp);
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
	fjmp(&fp, BPF_JMP | BPF_JEQ | BPF_K, 16 /* AF_NETLINK */, lnl, lrows);
	fplace(&fp, lnl);
	lerr = flabel(&fp);
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2]));
	fjmp(&fp, BPF_JMP | BPF_JEQ | BPF_K, 9 /* NETLINK_AUDIT */, lerr, lrows);
	fplace(&fp, lerr);
	fstmt(&fp, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 93 /* EPROTONOSUPPORT */);

	/* the rows: one JGE tree; a leaf of a row the lane lists branches to one of
	 * the shared cookie blocks, emitted after the tree */
	fplace(&fp, lrows);
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	lb.lc0 = flabel(&fp); lb.lc1 = flabel(&fp); lb.lop = flabel(&fp);
	frows(&fp, 0, policy_count());
	fstmt(&fp, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	fplace(&fp, lb.lc0); emit_cookie(&fp, 0);
	fplace(&fp, lb.lc1); emit_cookie(&fp, 1);
	fplace(&fp, lb.lop); emit_openat(&fp);

	if (fresolve(&fp) < 0)
		return -1;
	prog.len = (unsigned short) fp.n;
	prog.filter = fp.f;

	if (sys_prctl(PR_SET_NO_NEW_PRIVS, 1, 0) < 0)
		return -1;
	return sys_prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (i64) &prog);
}

/* ---- what the applier may use: registers and memory ---------------------- */
static int get_regs(struct tracee *t)
{
	struct iovec iov = { &t->r, sizeof t->r };

	t->dirty = 0;
	if (sys_ptrace(PTRACE_GETREGSET, t->pid, NT_PRSTATUS, (i64) &iov) < 0)
		return -1;
	t->scratch = t->r.sp - 0x200;            /* stay clear of anything near the stack pointer */
	return 0;
}

static int set_regs(struct tracee *t)
{
	struct iovec iov = { &t->r, sizeof t->r };

	return (int) sys_ptrace(PTRACE_SETREGSET, t->pid, NT_PRSTATUS, (i64) &iov);
}

ssize_t mon_read(struct tracee *t, u64 addr, void *buf, size_t n)
{
	struct iovec l = { buf, n }, r = { (void *) addr, n };

	return sys_vm_read(t->pid, &l, &r);
}

ssize_t mon_read_str(struct tracee *t, u64 addr, char *buf, size_t n)
{
	return mon_read(t, addr, buf, n);
}

u64 mon_scratch(struct tracee *t, const void *data, size_t n)
{
	struct iovec l = { (void *) data, n }, d;
	u64 dst = (t->scratch - n) & ~0xfUL;

	d.iov_base = (void *) dst;
	d.iov_len = n;
	if (sys_vm_write(t->pid, &l, &d) != (ssize_t) n)
		return 0;
	t->scratch = dst;                        /* the next call goes below this one */
	return dst;
}

ssize_t mon_write(struct tracee *t, u64 addr, const void *data, size_t n)
{
	struct iovec l = { (void *) data, n }, d = { (void *) addr, n };

	return sys_vm_write(t->pid, &l, &d);
}

void mon_skip(struct tracee *t, i64 value)
{
	t->r.regs[0] = (u64) value;
	t->dirty = 1;
	t->skip = 1;
}

void mon_set_arg(struct tracee *t, int i, u64 v)
{
	t->r.regs[i] = v;
	t->dirty = 1;
}

void mon_set_nr(struct tracee *t, int nr)
{
	struct iovec iov = { &nr, sizeof nr };

	sys_ptrace(PTRACE_SETREGSET, t->pid, NT_ARM_SYSTEM_CALL, (i64) &iov);
}

/* make the kernel skip the syscall the tracee is entering (arm64: syscall number -1) */
static void skip_syscall(struct tracee *t)
{
	int nr = -1;
	struct iovec iov = { &nr, sizeof nr };

	sys_ptrace(PTRACE_SETREGSET, t->pid, NT_ARM_SYSTEM_CALL, (i64) &iov);
}

/* ---- the platform killed a syscall: it arrives as SIGSYS ------------------- */
/* Answer it as the policy says.  The syscall was not run and the program counter
 * is already past the `svc`, so setting the result register is all it takes.
 * Returns 1 if this was the platform's filter, 0 for an ordinary SIGSYS. */
#ifndef DN_HOST_ADB
static int answer_sigsys(struct tracee *t)
{
	unsigned char si[128];
	unsigned nr;

	if (sys_ptrace(PTRACE_GETSIGINFO, t->pid, 0, (i64) si) < 0 || *(int *) (si + 8) != SYS_SECCOMP_CODE)
		return 0;
	if (get_regs(t) < 0)
		return 0;
	nr = (unsigned) t->r.regs[8];
	trace_denied(t);
	set_regs(t);
	n_sigsys++;
	if (nr < 512)
		deny_count[nr]++;
	return 1;
}
#endif

/* ---- the loop ---------------------------------------------------------- */
static int loop(pid_t root)
{
	int rc = 0;

	for (;;) {
		int st, how = PTRACE_CONT, deliver = 0;
		pid_t p = 0;
		struct tracee t;

		if (spin_ns) {                                    /* -s: poll for a while before blocking */
			i64 until = sys_monotonic_ns() + spin_ns;

			do
				p = sys_wait4(-1, &st, WALL | 1);  /* WNOHANG */
			while (p == 0 && sys_monotonic_ns() < until);
		}
		if (p == 0)
			p = sys_wait4(-1, &st, WALL);

		if (p < 0) {
			if (p == -EINTR)
				continue;
			break;
		}
		if (!WIFSTOPPED_(st)) {                           /* exited or killed */
			if (p == root)
				rc = WIFEXITED_(st) ? WEXITSTATUS_(st) : 128 + WTERMSIG_(st);
			if (mode == M_FULL)
				trace_gone(p);
			continue;
		}

		t.pid = p;
		t.dirty = 0;
		t.skip = 0;
		t.mark_exit = 0;
		t.dac_exit = 0;
		if (mode == M_FULL)
			trace_stop(p);
		{
			int sig = WSTOPSIG_(st), ev = st >> 16;

			if (ev == PTRACE_EVENT_SECCOMP) {          /* a caught syscall, at entry */
				unsigned long msg = 0;

				n_enter++;
				if (mode == M_FULL && get_regs(&t) == 0) {
					const struct row *rw = policy_row((unsigned) t.r.regs[8]);
					struct pid_state *s = pid_state_get(p, 1);

					/* the row says whether to stop at exit: no need for the event message */
					msg = rw ? rw->exit_stop : 0;
					s->cur_nr = (unsigned short) t.r.regs[8];
					s->saved0 = t.r.regs[0];
					if (rw)
						trace_enter(&t, rw);
					if (t.dirty)
						set_regs(&t);
					if (t.skip) {
						skip_syscall(&t);
						msg = 0;            /* answered at entry: nothing to rewrite at exit */
					}
					s->mark_exit = (unsigned char) (t.mark_exit && !t.skip);
					s->dac_exit = (unsigned char) (t.dac_exit && !t.skip);
					if (s->dac_exit) {
						s->dac_mode = t.dac_mode;
						msg |= 1;              /* stop at exit to restore the mode */
					}
					if (s->mark_exit)
						msg |= 1;              /* stop at exit to mark the new file */
				} else {
					sys_ptrace(PTRACE_GETEVENTMSG, p, 0, (i64) &msg);
					if (mode == M_REGS)
						get_regs(&t);
				}
				if (msg & 1) {
					EXP(p) = 1;
					how = PTRACE_SYSCALL;
				}
			} else if (sig == (SIGTRAP_ | 0x80)) {     /* the exit stop */
				struct pid_state *s = mode == M_FULL ? pid_state_get(p, 1) : 0;
				const struct row *rw = mode == M_FULL ? policy_row(s->cur_nr) : NULL;

				n_exit++;
				if (mode == M_FULL && get_regs(&t) == 0) {
					if (rw && (rw->result != R_NONE || rw->special == SP_SOCK_OUT)) {
						t.entry_arg0 = s->saved0;
						trace_exit(&t, rw);
					}
					t.mark_exit = s->mark_exit;
					s->mark_exit = 0;
					t.dac_exit = s->dac_exit;
					t.dac_mode = s->dac_mode;
					s->dac_exit = 0;
					if (t.mark_exit)
						trace_mark_owner(&t);
					if (t.dac_exit)
						trace_dac_restore(&t);
					if (t.dirty)
						set_regs(&t);
				}
				EXP(p) = 0;
			} else if (ev) {                           /* fork, vfork, clone, exec */
				n_event++;
				if (mode == M_FULL) {
					if (ev == PTRACE_EVENT_EXEC) {
						trace_exe_commit(p);
					} else if (ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK || ev == PTRACE_EVENT_CLONE) {
						unsigned long child = 0;

						sys_ptrace(PTRACE_GETEVENTMSG, p, 0, (i64) &child);
						trace_gone((pid_t) child);      /* a reused pid: nothing of the old process */
						trace_exe_fork(p, (pid_t) child);
					}
				}
				how = EXP(p) ? PTRACE_SYSCALL : PTRACE_CONT;
			} else {                                   /* a signal-delivery stop */
				n_sig++;
#ifndef DN_HOST_ADB
				if (sig == SIGSYS_ && mode == M_FULL && answer_sigsys(&t))
					deliver = 0;
				else
#endif
					deliver = (sig == SIGSTOP_ || sig == SIGTRAP_) ? 0 : sig;
				how = EXP(p) ? PTRACE_SYSCALL : PTRACE_CONT;
			}
		}
		sys_ptrace(how, p, 0, deliver);
	}
	return rc;
}

static void stats(i64 ns)
{
#ifndef DN_HOST_ADB
	unsigned i;
#endif

	put(2, "dn-tiny: enter="); putu(2, n_enter);
	put(2, " exit="); putu(2, n_exit);
	put(2, " event="); putu(2, n_event);
	put(2, " sig="); putu(2, n_sig);
	put(2, " paths="); putu(2, trace_n_rewritten);
	put(2, " answered="); putu(2, trace_n_answered);
	put(2, " execs="); putu(2, exec_n_rewritten);
	put(2, " sigsys="); putu(2, n_sigsys);
	put(2, "  "); putms(2, ns); put(2, "s  (filter "); putu(2, policy_count()); put(2, " syscalls)\n");
#ifndef DN_HOST_ADB
	if (n_sigsys) {
		put(2, "dn-tiny: answered for the platform's filter:");
		for (i = 0; i < 512; i++)
			if (deny_count[i]) { put(2, " "); putu(2, i); put(2, "x"); putu(2, deny_count[i]); }
		put(2, "\n");
	}
#endif
}

int main(int argc, char **argv)
{
	int i = 1, quiet = 0, st, rc, initial_root = 0;
	i64 t0, t1;
	const char *root = NULL;
	pid_t pid;

	for (; i < argc && argv[i][0] == '-' && strcmp(argv[i], "--") != 0; i++) {
		if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			i++;
			mode = !strcmp(argv[i], "bare") ? M_BARE : !strcmp(argv[i], "regs") ? M_REGS : M_FULL;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			root = argv[++i];
			policy_set_root(root);
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			sys_cpu_pin((int) atou(argv[++i]));
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			spin_ns = (i64) atou(argv[++i]) * 1000;
		} else if (!strcmp(argv[i], "--initial") && i + 1 < argc) {
			const char *p = argv[++i];

			if (!strcmp(p, "user"))
				initial_root = 0;
			else if (!strcmp(p, "root"))
				initial_root = 1;
			else {
				put(2, "dn-tiny: --initial is user or root\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "-q"))
			quiet = 1;
		else if (!strcmp(argv[i], "--dnexec"))
			exec_use_dnexec_set(1);
		else if (!strcmp(argv[i], "--no-dnexec"))
			exec_use_dnexec_set(0);
	}
	if (i < argc && !strcmp(argv[i], "--"))
		i++;
	if (i >= argc || (mode == M_FULL && root == NULL)) {
		put(2, "usage: dn-tiny [-m bare|regs|full] [-t ROOT] [--initial user|root] [-c CPU] [-s USEC] [-q] -- PROGRAM [ARG...]\n");
		return 2;
	}
	if (root != NULL)
		trace_synth_init();              /* the synthetic /proc files, before the tracee exists */

	pid = sys_fork();                        /* a raw clone: no libc, so no set_robust_list in the child */
	if (pid < 0) {
		put(2, "dn-tiny: clone failed\n");
		return 1;
	}
	if (pid == 0) {
		sys_default_sigpipe();
		sys_ptrace(PTRACE_TRACEME, 0, 0, 0);
		sys_kill(sys_getpid(), SIGSTOP_);        /* let the tracer set options first */
		if (install_filter() < 0) {
			put(2, "dn-tiny: seccomp failed\n");
			sys_exit(126);
		}
		sys_execv(argv[i], argv + i);
		put(2, "dn-tiny: cannot execute "); put(2, argv[i]); put(2, "\n");
		sys_exit(127);
	}

	{
		unsigned ru = (unsigned) sc(174, 0, 0, 0, 0, 0, 0);      /* getuid */
		unsigned rg = (unsigned) sc(176, 0, 0, 0, 0, 0, 0);      /* getgid */

		policy_set_ids(ru, rg);
		trace_identity_init(initial_root ? 0 : ru, initial_root ? 0 : rg);
	}
	sys_wait4(pid, &st, 0);                  /* the child's SIGSTOP */
	sys_ptrace(PTRACE_SETOPTIONS, pid, 0,
		   PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
		   PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL);
	sys_ptrace(PTRACE_CONT, pid, 0, 0);

	t0 = sys_monotonic_ns();
	rc = loop(pid);
	t1 = sys_monotonic_ns();

	if (!quiet)
		stats(t1 - t0);
	return rc;
}
