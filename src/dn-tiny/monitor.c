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
static unsigned deny_count[512];                 /* by syscall number: killed by the platform, answered here */
static unsigned char expect_exit[PID_SLOTS];      /* by pid */
#define EXP(p) expect_exit[PID_SLOT(p)]
static unsigned short cur_nr[PID_SLOTS];             /* by pid: the syscall to be seen again at exit */
static u64 saved0[PID_SLOTS];                     /* by pid: x0 as that syscall was entered */

/* ---- the filter: one TRACE rule per row -------------------------------- */
/* The program is built with labels, so no jump offset is counted by hand: a
 * conditional jump names the label it goes to when true and when false, and
 * resolve() turns them into offsets.  One JGE tree holds the rows: a number no row
 * lists is one tree away from ALLOW, and a row the lane also lists tests the shim's
 * cookie (args[5]) in its own leaf.  Only those rows read an argument, which is what
 * keeps the kernel (5.11 and later) from caching its answer per number. */
#define FNINS 1024                     /* BPF_MAXINSNS is 4096; this program is about 400 */
#define FNLAB 512
struct fjump { size_t pc; int lt, lf; };
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
	p->fx[p->nfx] = (struct fjump) { p->n, lt, lf };
	p->nfx++;
	fins(p, code, 0, 0, k);
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

/* the decision for a row the tree matched: the lane's cookie first, for a row the
 * shim answers, then TRACE (the cookie is wrong, or openat asks to write), or ALLOW */
static void emit_row(struct fprog *p, const struct row *rw)
{
	unsigned kind = rw->nr < 512 ? lane_kind[rw->nr] : 0;
	int lc2, lc3, lrow, lallow, lopend;

	if (!kind) {
		fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_TRACE | (rw->exit_stop & SECCOMP_RET_DATA));
		return;
	}
	lc2 = flabel(p); lrow = flabel(p);
	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]));
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_LO, lc2, lrow);
	fplace(p, lc2);
	lc3 = flabel(p);
	fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
	fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_HI, lc3, lrow);
	fplace(p, lc3);
	lallow = flabel(p);
	if (kind == 2) {
		fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
		lopend = flabel(p);
		fjmp(p, BPF_JMP | BPF_JEQ | BPF_K, DN_LANE_OPENAT, lopend, lallow);
		fplace(p, lopend);
		fstmt(p, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2]));
		fjmp(p, BPF_JMP | BPF_JSET | BPF_K, DN_LANE_OPEN_DENY, lrow, lallow);
	}
	fplace(p, lallow);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	fplace(p, lrow);
	fstmt(p, BPF_RET | BPF_K, SECCOMP_RET_TRACE | (rw->exit_stop & SECCOMP_RET_DATA));
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

	/* the rows: one JGE tree, a leaf testing the cookie for a row the lane lists */
	fplace(&fp, lrows);
	fstmt(&fp, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	frows(&fp, 0, policy_count());
	fstmt(&fp, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

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
			EXP(p) = 0;
			if (mode == M_FULL)
				trace_gone(p);
			continue;
		}

		t.pid = p;
		t.dirty = 0;
		t.skip = 0;
		if (mode == M_FULL)
			trace_stop(p);
		{
			int sig = WSTOPSIG_(st), ev = st >> 16;

			if (ev == PTRACE_EVENT_SECCOMP) {          /* a caught syscall, at entry */
				unsigned long msg = 0;

				n_enter++;
				if (mode == M_FULL && get_regs(&t) == 0) {
					const struct row *rw = policy_row((unsigned) t.r.regs[8]);

					/* the row says whether to stop at exit: no need for the event message */
					msg = rw ? rw->exit_stop : 0;
					cur_nr[PID_SLOT(p)] = (unsigned short) t.r.regs[8];
					saved0[PID_SLOT(p)] = t.r.regs[0];
					if (rw)
						trace_enter(&t, rw);
					if (t.dirty)
						set_regs(&t);
					if (t.skip) {
						skip_syscall(&t);
						msg = 0;            /* answered at entry: nothing to rewrite at exit */
					}
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
				const struct row *rw = mode == M_FULL ? policy_row(cur_nr[PID_SLOT(p)]) : NULL;

				n_exit++;
				if (rw && rw->result != R_NONE && get_regs(&t) == 0) {
					t.entry_arg0 = saved0[PID_SLOT(p)];
					trace_exit(&t, rw);
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
				if (sig == SIGSYS_ && mode == M_FULL && answer_sigsys(&t))
					deliver = 0;
				else
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
	unsigned i;

	put(2, "dn-tiny: enter="); putu(2, n_enter);
	put(2, " exit="); putu(2, n_exit);
	put(2, " event="); putu(2, n_event);
	put(2, " sig="); putu(2, n_sig);
	put(2, " paths="); putu(2, trace_n_rewritten);
	put(2, " answered="); putu(2, trace_n_answered);
	put(2, " execs="); putu(2, exec_n_rewritten);
	put(2, " sigsys="); putu(2, n_sigsys);
	put(2, "  "); putms(2, ns); put(2, "s  (filter "); putu(2, policy_count()); put(2, " syscalls)\n");
	if (n_sigsys) {
		put(2, "dn-tiny: answered for the platform's filter:");
		for (i = 0; i < 512; i++)
			if (deny_count[i]) { put(2, " "); putu(2, i); put(2, "x"); putu(2, deny_count[i]); }
		put(2, "\n");
	}
}

int main(int argc, char **argv)
{
	int i = 1, quiet = 0, st, rc;
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
		} else if (!strcmp(argv[i], "-q"))
			quiet = 1;
	}
	if (i < argc && !strcmp(argv[i], "--"))
		i++;
	if (i >= argc || (mode == M_FULL && root == NULL)) {
		put(2, "usage: dn-tiny [-m bare|regs|full] [-t ROOT] [-c CPU] [-s USEC] [-q] -- PROGRAM [ARG...]\n");
		return 2;
	}

	{                                        /* the per-process tables (mon.h) and this kernel's pids */
		char b[16] = { 0 };
		int fd = sys_open("/proc/sys/kernel/pid_max", 0);

		if (fd >= 0) {
			sys_read(fd, b, sizeof b - 1);
			sys_close(fd);
			if (atou(b) > PID_SLOTS) {
				put(2, "dn-tiny: pid_max is ");
				put(2, b);
				put(2, "  two processes whose pids differ by 65536 would share their state\n");
			}
		}
	}
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

	policy_set_ids((unsigned) sc(174, 0, 0, 0, 0, 0, 0), (unsigned) sc(176, 0, 0, 0, 0, 0, 0));   /* getuid, getgid */
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
