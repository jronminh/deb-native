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
static unsigned char expect_exit[1 << 16];       /* by pid, enough here */
#define EXP(p) expect_exit[(p) & 0xffff]
static unsigned short cur_nr[1 << 16];           /* by pid: the syscall to be seen again at exit */
static u64 saved0[1 << 16];                      /* by pid: x0 as that syscall was entered */

/* ---- the filter: one TRACE rule per row -------------------------------- */
static int install_filter(void)
{
	struct sock_filter f[2 * 512 + 8];
	struct sock_fprog prog = { 0, f };
	size_t i, n = 0;

	f[n++] = (struct sock_filter) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
	f[n++] = (struct sock_filter) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
	f[n++] = (struct sock_filter) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	f[n++] = (struct sock_filter) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	for (i = 0; i < policy_count(); i++) {
		const struct row *rw = policy_at(i);

		f[n++] = (struct sock_filter) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, rw->nr, 0, 1);
		f[n++] = (struct sock_filter) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE | (rw->exit_stop & SECCOMP_RET_DATA));
	}
	f[n++] = (struct sock_filter) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
	prog.len = (unsigned short) n;

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
					cur_nr[p & 0xffff] = (unsigned short) t.r.regs[8];
					saved0[p & 0xffff] = t.r.regs[0];
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
				const struct row *rw = mode == M_FULL ? policy_row(cur_nr[p & 0xffff]) : NULL;

				n_exit++;
				if (rw && rw->result != R_NONE && get_regs(&t) == 0) {
					t.entry_arg0 = saved0[p & 0xffff];
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
