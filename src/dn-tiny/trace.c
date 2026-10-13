/* trace.c -- the applier.  It has no `if (syscall == ...)`: the row says where
 * the path is or what to fake, the policy says what it becomes, the monitor
 * moves the bytes. */
#include "exec.h"
#include "trace.h"

unsigned long trace_n_rewritten;

static void rewrite_path(struct tracee *t, int arg)
{
	char in[256], out[512];
	ssize_t k = mon_read_str(t, t->r.regs[arg], in, sizeof in);
	u64 at;
	int n, nf;

	if (k <= 0 || memchr(in, 0, (size_t) k) == NULL)
		return;
	nf = policy_nofollow((unsigned) t->r.regs[8], arg, (const u64 *) t->r.regs);
	if (in[0] == '/' || in[0] == 0) {
		n = policy_path_nf(in, out, sizeof out, nf);
	} else {                                        /* from the cwd or a directory fd: ask the kernel where that is */
		char base[256], link[48] = "/proc/";
		int da = policy_dirfd_arg((unsigned) t->r.regs[8], arg);
		int fd = da < 0 ? -100 : (int) t->r.regs[da];
		size_t i = 6;
		pid_t pid = t->pid;
		char dig[12];
		int nd = 0;
		ssize_t b;

		do { dig[nd++] = (char) ('0' + pid % 10); pid /= 10; } while (pid);
		while (nd)
			link[i++] = dig[--nd];
		if (fd == -100) {
			scpy(link + i, sizeof link - i, "/cwd");
		} else {
			unsigned f = (unsigned) fd;

			scpy(link + i, sizeof link - i, "/fd/");
			i += 4;
			do { dig[nd++] = (char) ('0' + f % 10); f /= 10; } while (f);
			while (nd)
				link[i++] = dig[--nd];
			link[i] = 0;
		}
		b = sys_readlink(link, base, sizeof base - 1);
		if (b <= 0)
			return;
		base[b] = 0;
		n = policy_path_at(base, in, out, sizeof out, nf);
	}
	if (n == 0)
		return;
	at = mon_scratch(t, out, (size_t) n);
	if (at == 0)
		return;
	mon_set_arg(t, arg, at);
	trace_n_rewritten++;
}

/* The ids a process was last given.  Everything starts as root (0); a process that
 * drops privilege, as apt's download method does, then reads back what it set. */
#define EXE_SLOTS_IDS (1 << 14)
struct ids { unsigned ru, eu, su, rg, eg, sg; };
static struct ids fake_ids[EXE_SLOTS_IDS];

static struct ids *ids_of(pid_t pid) { return &fake_ids[(unsigned) pid % EXE_SLOTS_IDS]; }

static void set_id(unsigned *r, unsigned *e, unsigned *s, long a, long b, long c, int mode)
{
	if (mode == 0) {                               /* setuid, as root: all three */
		*r = *e = *s = (unsigned) a;
	} else if (mode == 1) {                        /* setreuid */
		if (a != -1) *r = (unsigned) a;
		if (b != -1) *e = (unsigned) b;
		*s = *e;
	} else {                                       /* setresuid */
		if (a != -1) *r = (unsigned) a;
		if (b != -1) *e = (unsigned) b;
		if (c != -1) *s = (unsigned) c;
	}
}

/* fake root: answer without running the syscall */
static void fake_result(struct tracee *t, int fake)
{
	unsigned zero[3] = { 0, 0, 0 };

	switch (fake) {
	case F_RESIDS: {                                /* getresuid(&r, &e, &s), getresgid */
		struct ids *d = ids_of(t->pid);
		int g = t->r.regs[8] == 150;

		zero[0] = g ? d->rg : d->ru;
		zero[1] = g ? d->eg : d->eu;
		zero[2] = g ? d->sg : d->su;
		mon_write(t, t->r.regs[0], &zero[0], 4);
		mon_write(t, t->r.regs[1], &zero[1], 4);
		mon_write(t, t->r.regs[2], &zero[2], 4);
		mon_skip(t, 0);
		break;
	}
	case F_GETID: {
		struct ids *d = ids_of(t->pid);

		mon_skip(t, t->r.regs[8] == 174 ? d->ru : t->r.regs[8] == 175 ? d->eu :
			    t->r.regs[8] == 176 ? d->rg : d->eg);
		break;
	}
	case F_SETID: {
		struct ids *d = ids_of(t->pid);
		long a = (long) (int) t->r.regs[0], b = (long) (int) t->r.regs[1], c = (long) (int) t->r.regs[2];
		unsigned long nr = t->r.regs[8];

		if (nr == 146)      set_id(&d->ru, &d->eu, &d->su, a, 0, 0, 0);       /* setuid */
		else if (nr == 144) set_id(&d->rg, &d->eg, &d->sg, a, 0, 0, 0);       /* setgid */
		else if (nr == 145) set_id(&d->ru, &d->eu, &d->su, a, b, 0, 1);       /* setreuid */
		else if (nr == 143) set_id(&d->rg, &d->eg, &d->sg, a, b, 0, 1);       /* setregid */
		else if (nr == 147) set_id(&d->ru, &d->eu, &d->su, a, b, c, 2);       /* setresuid */
		else if (nr == 149) set_id(&d->rg, &d->eg, &d->sg, a, b, c, 2);       /* setresgid */
		/* setfsuid, setfsgid: the answer is the previous value, the effective id here */
		mon_skip(t, nr == 151 ? d->eu : nr == 152 ? d->eg : 0);
		break;
	}
	case F_GROUPS:                                  /* getgroups(size, list): one group, 0 */
		if (t->r.regs[0] != 0)
			mon_write(t, t->r.regs[1], zero, 4);
		mon_skip(t, 1);
		break;
	case F_EXDEV:                                   /* link across file systems */
		mon_skip(t, -18);
		break;
	default:
		mon_skip(t, 0);
	}
}

/* The platform denies hard links (SELinux, not seccomp), so a link is made as a copy: the
 * program asked for a second name for a file, and what it nearly always does next is remove
 * or rename the first (git's object store, ccache, dpkg's backups), which a symlink to the
 * first name would not survive.  What a copy does not keep is that the two names are one
 * file (a later write to one is not seen by the other, st_nlink stays 1).  A symlink is
 * copied as a symlink, a directory is refused as link(2) refuses it. */
#define LINK_BUF 65536

static i64 link_copy(const char *old, const char *new)
{
	static char buf[LINK_BUF];
	u64 st[18];                                      /* struct stat: 128 bytes */
	unsigned mode;
	i64 in, out, r;

	r = sc(79, -100, (i64) old, (i64) st, 0x100 /* AT_SYMLINK_NOFOLLOW */, 0, 0);
	if (r < 0)
		return r;
	mode = *(unsigned *) ((char *) st + 16);
	if ((mode & 0xf000) == 0x4000)
		return -1;                               /* EPERM: a directory */
	if ((mode & 0xf000) == 0xa000) {                 /* a symlink: the same text under the new name */
		r = sys_readlink(old, buf, sizeof buf - 1);
		if (r <= 0)
			return r < 0 ? r : -2;
		buf[r] = 0;
		return sc(36, (i64) buf, -100, (i64) new, 0, 0, 0);
	}
	if ((mode & 0xf000) != 0x8000)
		return -1;                               /* a device, fifo or socket */
	in = sc(56, -100, (i64) old, 0x80000 /* O_RDONLY | O_CLOEXEC */, 0, 0, 0);
	if (in < 0)
		return in;
	out = sc(56, -100, (i64) new, 0x80000 | 1 | 0x40 | 0x80 /* O_WRONLY | O_CREAT | O_EXCL */, mode & 07777, 0, 0);
	if (out < 0) {
		sys_close((int) in);
		return out;
	}
	for (;;) {
		i64 n = sys_read((int) in, buf, sizeof buf), off = 0;

		if (n < 0) { r = n; break; }
		if (n == 0) { r = 0; break; }
		while (off < n) {
			i64 w = sys_write((int) out, buf + off, (size_t) (n - off));

			if (w <= 0) { r = w < 0 ? w : -5; goto done; }
			off += w;
		}
	}
done:
	if (r == 0)
		r = sc(52, out, mode & 07777, 0, 0, 0, 0);   /* fchmod: the umask must not narrow it */
	sys_close((int) in);
	sys_close((int) out);
	if (r < 0)
		sc(35, -100, (i64) new, 0, 0, 0, 0);     /* unlinkat: no half-written copy is left */
	return r;
}

/* linkat(od, old, nd, new, flags) with both names already mapped: do the copy here and
 * answer the call. */
static void link_by_copy(struct tracee *t)
{
	char old[512], new[512];
	ssize_t a = mon_read_str(t, t->r.regs[1], old, sizeof old - 1);
	ssize_t b = mon_read_str(t, t->r.regs[3], new, sizeof new - 1);
	i64 r;

	if (a <= 0 || b <= 0 || memchr(old, 0, (size_t) a) == NULL || memchr(new, 0, (size_t) b) == NULL)
		return;
	if (old[0] != '/' || new[0] != '/')              /* a name outside the prefix, left to the kernel */
		return;
	r = link_copy(old, new);
	mon_skip(t, r < 0 ? r : 0);
}

void trace_enter(struct tracee *t, const struct row *rw)
{
	if (rw->fake != F_NONE) {
		fake_result(t, rw->fake);
		return;
	}
	if (rw->special == SP_EXEC) {
		exec_enter(t);
		return;
	}
	if (rw->path_arg >= 0)
		rewrite_path(t, rw->path_arg);
	if (rw->path_arg2 >= 0)
		rewrite_path(t, rw->path_arg2);
	if (rw->special == SP_LINK)             /* linkat: the platform denies it, so it is a copy */
		link_by_copy(t);
}

/* ---- results: what the program is told, not what the kernel returned --------- */

#define EXE_SLOTS (1 << 14)
#define EXE_LEN   128
static char exe_now[EXE_SLOTS][EXE_LEN];          /* by pid, the program each process runs */
static char exe_next[EXE_SLOTS][EXE_LEN];         /* noted at execve entry, valid once it succeeds */

void trace_exe_pending(pid_t pid, const char *guest_path)
{
	scpy(exe_next[(unsigned) pid % EXE_SLOTS], EXE_LEN, guest_path);
}

void trace_exe_commit(pid_t pid)
{
	unsigned i = (unsigned) pid % EXE_SLOTS;

	memcpy(exe_now[i], exe_next[i], EXE_LEN);
}

void trace_exe_fork(pid_t parent, pid_t child)
{
	*ids_of(child) = *ids_of(parent);
	memcpy(exe_now[(unsigned) child % EXE_SLOTS], exe_now[(unsigned) parent % EXE_SLOTS], EXE_LEN);
}

/* "/proc/self/exe" or "/proc/<pid>/exe": the pid it names, or 0 if it is neither */
static pid_t proc_exe_pid(const char *p, pid_t self)
{
	pid_t v = 0;
	size_t n = strlen(p);

	if (strncmp(p, "/proc/", 6) != 0 || n < 10 || strcmp(p + n - 4, "/exe") != 0)
		return 0;
	p += 6;
	if (strncmp(p, "self/exe", 9) == 0)
		return self;
	for (; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	return strcmp(p, "/exe") == 0 ? v : 0;
}

/* getcwd(buf, size): the length including the NUL is in x0, the buffer was x0 */
static void fix_cwd(struct tracee *t)
{
	char b[512], g[512];
	i64 n = (i64) t->r.regs[0];
	int k;

	if (n <= 0 || n > (i64) sizeof b || mon_read(t, t->entry_arg0, b, (size_t) n) != n || b[n - 1] != '\0')
		return;
	k = policy_unpath(b, g, sizeof g);
	if (k == 0 || mon_write(t, t->entry_arg0, g, (size_t) k) != k)
		return;
	t->r.regs[0] = (u64) k;
	t->dirty = 1;
}

/* "/proc/self/..." as the tracer must spell it to name the tracee's own entry */
static void self_to_pid(const char *in, char *out, size_t cap, pid_t pid)
{
	char dig[12];
	size_t i = 0;
	int nd = 0;

	if (strncmp(in, "/proc/self/", 11) != 0) {
		scpy(out, cap, in);
		return;
	}
	do { dig[nd++] = (char) ('0' + pid % 10); pid /= 10; } while (pid);
	memcpy(out, "/proc/", 6);
	i = 6;
	while (nd && i + 1 < cap)
		out[i++] = dig[--nd];
	scpy(out + i, cap - i, in + 10);
}

/* readlinkat(dirfd, path, buf, size): the length (no NUL) is in x0 */
static void fix_link(struct tracee *t)
{
	char path[256], b[512], g[512];
	i64 n = (i64) t->r.regs[0];
	u64 buf = t->r.regs[2], size = t->r.regs[3];
	ssize_t k = mon_read_str(t, t->r.regs[1], path, sizeof path - 1);
	pid_t who;
	int m;

	if (n <= 0 || n > (i64) sizeof b - 1)
		return;
	if (k > 0 && (path[k] = '\0', 1) && (who = proc_exe_pid(path, t->pid)) != 0) {
		const char *exe = exe_now[(unsigned) who % EXE_SLOTS];

		m = (int) strlen(exe);
		if (m == 0)
			return;
		if ((u64) m > size)
			m = (int) size;
		if (mon_write(t, buf, exe, (size_t) m) != m)
			return;
		t->r.regs[0] = (u64) m;
		t->dirty = 1;
		return;
	}
	if ((u64) n == size) {          /* the buffer is full: the text may be cut, and mapping a cut text back
					 * gives a short one that looks complete; read the whole link here */
		char p2[300];
		ssize_t r;

		if (k <= 0 || path[0] != '/')
			return;
		self_to_pid(path, p2, sizeof p2, t->pid);
		r = sys_readlink(p2, b, sizeof b - 1);
		if (r <= 0)
			return;
		n = r;
	} else if (mon_read(t, buf, b, (size_t) n) != n) {
		return;
	}
	b[n] = '\0';
	m = policy_unpath(b, g, sizeof g);
	if (m == 0)
		return;
	m--;                                             /* the link text has no NUL */
	if ((u64) m > size)
		m = (int) size;                          /* the program's buffer is smaller: it is told so */
	if (mon_write(t, buf, g, (size_t) m) != m)
		return;
	t->r.regs[0] = (u64) m;
	t->dirty = 1;
}

/* the owner in a stat (uid at 24, gid at 28) or statx (uid at 20, gid at 24): the real ids read as root */
static void fix_owner(struct tracee *t, u64 buf, unsigned off)
{
	unsigned id[2];

	if (t->r.regs[0] != 0 || mon_read(t, buf + off, id, sizeof id) != (ssize_t) sizeof id)
		return;
	id[0] = policy_map_uid(id[0]);
	id[1] = policy_map_gid(id[1]);
	mon_write(t, buf + off, id, sizeof id);
}

/* A symlink's size is the length of its target.  On an encrypted file system the kernel
 * reports a padded size, and the target the program reads has been mapped back to the
 * guest's spelling, so the size is set to what readlink will say (dpkg-realpath checks). */
static void fix_link_size(struct tracee *t, u64 buf, unsigned mode_off, unsigned mode_len, unsigned size_off)
{
	unsigned mode = 0;
	char path[256], host[256], guest[256];
	ssize_t k, r;
	u64 size;

	if (t->r.regs[0] != 0 || mon_read(t, buf + mode_off, &mode, mode_len) != (ssize_t) mode_len)
		return;
	if ((mode & 0xf000) != 0xa000)
		return;
	k = mon_read_str(t, t->r.regs[1], path, sizeof path);
	if (k <= 0 || path[0] != '/' || memchr(path, 0, (size_t) k) == NULL)
		return;
	r = sys_readlink(path, host, sizeof host - 1);
	if (r <= 0)
		return;
	host[r] = 0;
	size = (u64) r;
	if (policy_unpath(host, guest, sizeof guest) != 0)
		size = (u64) strlen(guest);
	mon_write(t, buf + size_off, &size, 8);
}

void trace_exit(struct tracee *t, const struct row *rw)
{
	switch (rw->result) {
	case R_CWD:   fix_cwd(t); break;
	case R_LINK:  fix_link(t); break;
	case R_STAT:
		fix_owner(t, t->r.regs[rw->result_arg], 24);
		if (t->r.regs[8] == 79)                    /* newfstatat: a path, not an fd */
			fix_link_size(t, t->r.regs[rw->result_arg], 16, 4, 48);
		break;
	case R_STATX:
		fix_owner(t, t->r.regs[rw->result_arg], 20);
		fix_link_size(t, t->r.regs[rw->result_arg], 28, 2, 40);
		break;
	default: break;
	}
}

/* The platform's filter killed this syscall (the tracee is at the SIGSYS stop,
 * its program counter already past the `svc`).  Set the registers so that, once
 * resumed, the program sees what the policy says.  A substitute is run by moving
 * the program counter back to the `svc` with the new number: it goes through our
 * filter like any other call, so its path is mapped too. */
void trace_denied(struct tracee *t)
{
	struct deny d;
	unsigned nr = (unsigned) t->r.regs[8];
	const struct row *rw = policy_row(nr);

	/* A call the platform kills never reaches the entry stop (seccomp takes the strictest
	 * answer, and TRAP outranks TRACE), so a faked call is answered here the same way. */
	if (rw && rw->fake != F_NONE) {
		fake_result(t, rw->fake);
		return;
	}
	policy_deny(nr, &d);
	if (d.kind == D_OPENAT2) {                      /* struct open_how { flags, mode, resolve } */
		u64 how[3] = { 0, 0, 0 };
		ssize_t k = mon_read(t, t->r.regs[2], how, sizeof how);

		if (k < 16 || (k >= 24 && how[2] != 0)) {      /* RESOLVE_* cannot be honoured by openat */
			t->r.regs[0] = (u64) -38;
			return;
		}
		t->r.regs[2] = how[0];
		t->r.regs[3] = how[1];
		t->r.regs[8] = 56;                      /* openat */
		t->r.pc -= 4;
		return;
	}
	if (d.kind == D_SUBST) {
		if (d.flags_arg >= 0 && (t->r.regs[d.flags_arg] & ~d.ok_mask)) {
			t->r.regs[0] = (u64) -38;       /* the program's own fallback does it */
			return;
		}
		if (d.zero_from >= 0)
			for (int i = d.zero_from; i < 6; i++)
				t->r.regs[i] = 0;
		t->r.regs[8] = d.to;
		t->r.pc -= 4;
		return;
	}
	t->r.regs[0] = (u64) d.value;
}
