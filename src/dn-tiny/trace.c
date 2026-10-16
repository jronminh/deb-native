/* trace.c -- the applier.  It has no `if (syscall == ...)`: the row says where
 * the path is or what to fake, the policy says what it becomes, the monitor
 * moves the bytes. */
#include "exec.h"
#include "trace.h"

unsigned long trace_n_rewritten;

/* ---- the synthetic /proc mount files: the rows in the table point /proc/mounts
 * (and mountinfo, /etc/mtab) at a file under var/lib/deb-native, so this only
 * writes that file, once, at boot.  A regular file is read with no stop; a memfd
 * would do too, but it cannot be reached by the child across the ptrace boundary
 * (/proc/<tracerpid>/fd/N answers EACCES). ------------------------------------ */
void trace_synth_init(void)
{
	static const char *const name[2] = { "mounts", "mountinfo" };
	const char *root = policy_root();
	char b[8192], p[600];
	int i;

	if (!root || policy_root_len() == 0)
		return;
	for (i = 0; i < 2; i++) {
		int len = policy_mounts(b, sizeof b, i);
		int fd;

		if (len <= 0)
			return;
		if (cat2(p, sizeof p, root, "/var/lib/deb-native/") < 0 ||
		    cat2(p, sizeof p, p, name[i]) < 0)
			return;
		fd = (int) sc(56, -100, (i64) p, 0101 /* O_WRONLY|O_CREAT */ | 01000 /* O_TRUNC */, 0600, 0, 0);
		if (fd < 0)
			return;
		sys_write(fd, b, (size_t) len);
		sys_close(fd);
	}
}

/* ---- copies the tracer keeps, and what drops them ---------------------------- */

/* The working directory of a process, as /proc/<pid>/cwd said.  Valid while the pid,
 * the cwd generation and the link generation are the ones it was read under: a chdir
 * by any process moves the first (threads share a cwd), a rename or an unlink of a
 * directory the second. */
#define CWD_SLOTS 4096
#define CWD_LEN   256
struct cwdc { pid_t pid; unsigned gen, lgen; char path[CWD_LEN]; };
static struct cwdc cwd_cache[CWD_SLOTS];
static unsigned cwd_gen = 1;
static unsigned char pending[PID_SLOTS];         /* by pid: the effects of the call in flight */

static void drop(int effect)
{
	if (effect & E_LINKS)
		policy_links_changed();
	if (effect & E_CWD)
		cwd_gen++;
}

void trace_stop(pid_t pid)
{
	unsigned char *e = &pending[PID_SLOT(pid)];

	if (*e) {                                       /* the call is over: what it changed is visible now */
		drop(*e);
		*e = 0;
	}
}

void trace_gone(pid_t pid)
{
	trace_stop(pid);
	cwd_cache[(unsigned) pid % CWD_SLOTS].pid = 0;
}

/* /proc/<pid>/cwd, from the cache when it holds it */
static ssize_t cwd_of(pid_t pid, const char *link, char *buf, size_t n)
{
	struct cwdc *c = &cwd_cache[(unsigned) pid % CWD_SLOTS];
	unsigned lg = policy_links_gen();
	size_t len;
	ssize_t b;

	if (c->pid == pid && c->gen == cwd_gen && c->lgen == lg) {
		len = strlen(c->path);
		if (len < n) {
			memcpy(buf, c->path, len);
			return (ssize_t) len;
		}
	}
	b = sys_readlink(link, buf, n);
	if (b > 0 && (size_t) b < CWD_LEN) {
		memcpy(c->path, buf, (size_t) b);
		c->path[b] = 0;
		c->pid = pid;
		c->gen = cwd_gen;
		c->lgen = lg;
	}
	return b;
}

/* The host path of the directory a name in register `arg` is taken from: its
 * directory fd, or the working directory.  1, or 0 (it can't be named). */
static int base_of(struct tracee *t, int arg, char *base, size_t cap)
{
	char link[48] = "/proc/";
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
	b = fd == -100 ? cwd_of(t->pid, link, base, cap) : sys_readlink(link, base, cap);
	if (b <= 0)
		return 0;
	base[b] = 0;
	return 1;
}

/* The guest absolute path a name in register `arg` would be, for the row rules
 * (EXDEV, `ro`) that are decided on the guest path before it is mapped.  1, or 0. */
static int guest_abs(struct tracee *t, int arg, char *out, size_t cap)
{
	char in[256], base[256], g[256];
	ssize_t k = mon_read_str(t, t->r.regs[arg], in, sizeof in - 1);

	if (k <= 0)
		return 0;
	in[k] = '\0';
	if (in[0] == '/') {
		scpy(out, cap, in);
		return 1;
	}
	if (in[0] == '\0' || !base_of(t, arg, base, sizeof base - 1))
		return 0;
	if (policy_unpath(base, g, sizeof g) == 0)
		return 0;
	if (cat2(out, cap, g, "/") < 0)
		return 0;
	return cat2(out, cap, out, in) >= 0 ? 1 : 0;
}

static void rewrite_path(struct tracee *t, int arg)
{
	char in[256], out[512], base[256];
	ssize_t k = mon_read_str(t, t->r.regs[arg], in, sizeof in);
	u64 at;
	int n, nf;

	if (k <= 0 || memchr(in, 0, (size_t) k) == NULL)
		return;
	nf = policy_nofollow((unsigned) t->r.regs[8], arg, (const u64 *) t->r.regs);
	if (in[0] == '/' || in[0] == 0) {
		n = policy_path_nf(in, out, sizeof out, nf);
	} else {                                        /* from the cwd or a directory fd: ask the kernel where that is */
		if (!base_of(t, arg, base, sizeof base - 1))
			return;
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

/* The path in an AF_UNIX sockaddr_un, mapped like any other path: guest->host on
 * the way in, host->guest on the answer (`out`).  The sockaddr register is
 * rw->path_arg; rw->path_arg2 is the length register (a value for bind/connect/
 * sendto, a pointer for the calls that fill it) or -1 for a msghdr, whose
 * msg_name is at offset 0 and msg_namelen at 8. */
static void sock_map(struct tracee *t, const struct row *rw, int out)
{
	struct { unsigned short fam; char path[108]; } sa;
	u64 nb, lp = 0, msg = 0;
	int mlp = 0;

	if (rw->path_arg2 >= 0) {
		nb = t->r.regs[rw->path_arg];
		lp = t->r.regs[rw->path_arg2];
	} else {
		u64 name;

		msg = t->r.regs[rw->path_arg];
		if (mon_read(t, msg, &name, 8) != 8 || name == 0)
			return;
		nb = name;
		mlp = 1;
	}
	if (nb == 0 || mon_read(t, nb, &sa, sizeof sa) != sizeof sa)
		return;
	if (sa.fam != 1)                               /* AF_UNIX */
		return;
	sa.path[sizeof sa.path - 1] = '\0';
	if (sa.path[0] == '\0')                        /* abstract or empty: no path */
		return;
	if (!out) {
		char host[420];
		unsigned nl;

		if (sa.path[0] != '/' || policy_path_nf(sa.path, host, sizeof host, 0) <= 0)
			return;                        /* already a host path */
		if (strlen(host) + 1 > sizeof sa.path)
			return;                        /* too long for sun_path */
		if (mon_write(t, nb + 2, host, strlen(host) + 1) != (ssize_t) strlen(host) + 1)
			return;
		nl = 2 + (unsigned) strlen(host) + 1;
		if (mlp)
			mon_write(t, msg + 8, &nl, 4);
		else
			mon_set_arg(t, rw->path_arg2, nl);
	} else {
		char guest[120];
		int m = policy_unpath(sa.path, guest, sizeof guest);
		unsigned nl;

		if (m == 0)                            /* not under a row: leave it */
			return;
		if (mon_write(t, nb + 2, guest, strlen(guest) + 1) != (ssize_t) strlen(guest) + 1)
			return;
		nl = 2 + (unsigned) strlen(guest) + 1;
		if (mlp)
			mon_write(t, msg + 8, &nl, 4);
		else
			mon_write(t, lp, &nl, 4);      /* the length is a pointer on the way out */
	}
}

/* The ids a process was last given.  Everything starts as root (0); a process that
 * drops privilege, as apt's download method does, then reads back what it set. */
struct ids { unsigned ru, eu, su, rg, eg, sg; };
static struct ids fake_ids[PID_SLOTS];

static struct ids *ids_of(pid_t pid) { return &fake_ids[PID_SLOT(pid)]; }

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

/* ---- hard links: Android refuses them and linkat makes a copy.  The copies of one
 * file are remembered here, so a stat says the link count a real link would (shadow
 * locks its files by linking a temporary name and checking for a count of 2); an
 * unlink drops the name it removes.  One table for every process of the session.
 * The copies do not share their contents: a write to one is not seen in the other. */
#define CL_MAX 64
static struct { u64 dev, ino; int group; } copies[CL_MAX];
static int n_copies, next_group = 1;

static int cl_find(u64 dev, u64 ino)
{
	int i;

	for (i = 0; i < n_copies; i++)
		if (copies[i].dev == dev && copies[i].ino == ino)
			return i;
	return -1;
}

static void cl_put(u64 dev, u64 ino, int group)
{
	if (n_copies == CL_MAX) {                       /* full: forget the oldest */
		memmove(&copies[0], &copies[1], sizeof copies[0] * (CL_MAX - 1));
		n_copies--;
	}
	copies[n_copies].dev = dev;
	copies[n_copies].ino = ino;
	copies[n_copies].group = group;
	n_copies++;
}

static void cl_add(u64 odev, u64 oino, u64 ndev, u64 nino)
{
	int i = cl_find(odev, oino), g;

	if (i >= 0) {
		g = copies[i].group;
	} else {
		g = next_group++;
		cl_put(odev, oino, g);
	}
	cl_put(ndev, nino, g);
}

static void cl_drop(u64 dev, u64 ino)
{
	int i = cl_find(dev, ino);

	if (i < 0)
		return;
	memmove(&copies[i], &copies[i + 1], sizeof copies[0] * (size_t) (n_copies - i - 1));
	n_copies--;
}

/* the link count of (dev, ino) if it is one of the copies, else 0 */
static unsigned cl_count(u64 dev, u64 ino)
{
	int i = cl_find(dev, ino), j;
	unsigned n = 0;

	if (i < 0)
		return 0;
	for (j = 0; j < n_copies; j++)
		n += copies[j].group == copies[i].group;
	return n;
}

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
	if (r == 0) {
		u64 nst[18];

		if (sc(80, out, (i64) nst, 0, 0, 0, 0) == 0)
			cl_add(st[0], st[1], nst[0], nst[1]);
	}
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

static void answer_at_entry(struct tracee *t, const struct row *rw);
static void unlink_copy(struct tracee *t);

/* the path in register `arg`, if it is absolute (a guest path we can place in the
 * table); 1, and the string in out, or 0 */
static int abs_guest(struct tracee *t, int arg, char *out, size_t cap)
{
	ssize_t k = mon_read_str(t, t->r.regs[arg], out, cap - 1);

	if (k <= 0)
		return 0;
	out[k] = '\0';
	return out[0] == '/';
}

void trace_enter(struct tracee *t, const struct row *rw)
{
	if (rw->effect) {                               /* drop now, and again once the call is over */
		drop(rw->effect);
		pending[PID_SLOT(t->pid)] |= (unsigned char) rw->effect;
	}
	if (rw->special == SP_SOCK_IN) {                /* an AF_UNIX path, in a sockaddr */
		sock_map(t, rw, 0);
		return;
	}
	if (rw->special == SP_SOCK_OUT)                 /* the kernel fills it; mapped at exit */
		return;
	if (rw->nr == 8 || rw->nr == 9 || rw->nr == 10) {   /* getxattr: the SELinux label is absent */
		char nm[64];
		ssize_t k = mon_read_str(t, t->r.regs[1], nm, sizeof nm - 1);

		if (k > 0 && (nm[k] = '\0', 1) && strcmp(nm, "security.selinux") == 0) {
			mon_skip(t, -61);               /* ENODATA */
			return;
		}
	}
	if (rw->fake != F_NONE) {
		fake_result(t, rw->fake);
		return;
	}
	if (rw->special == SP_EXEC) {
		exec_enter(t);
		return;
	}
	if (rw->nr == 38 || rw->nr == 276 || rw->nr == 37) {   /* rename/link: EXDEV between two rows */
		char a[256], b[256];

		if (abs_guest(t, 1, a, sizeof a) && abs_guest(t, 3, b, sizeof b) && !policy_same_row(a, b)) {
			mon_skip(t, -18);
			return;
		}
	}
	if (rw->path_arg >= 0 &&
	    (policy_writes(rw->nr) || (rw->nr == 56 && (t->r.regs[2] & 0x643)))) {   /* write to a `ro` row */
		char g[512];

		if (guest_abs(t, rw->path_arg, g, sizeof g) && policy_row_ro(g)) {
			mon_skip(t, -30);
			return;
		}
	}
	if (rw->path_arg2 >= 0 && policy_writes(rw->nr)) {
		char g[512];

		if (guest_abs(t, rw->path_arg2, g, sizeof g) && policy_row_ro(g)) {
			mon_skip(t, -30);
			return;
		}
	}
	if (rw->path_arg >= 0)
		rewrite_path(t, rw->path_arg);
	if (rw->path_arg2 >= 0)
		rewrite_path(t, rw->path_arg2);
	if (rw->special == SP_LINK)             /* linkat: the platform denies it, so it is a copy */
		link_by_copy(t);
	if (rw->nr == 35 && n_copies && !(t->r.regs[2] & 0x200))   /* unlinkat, not AT_REMOVEDIR */
		unlink_copy(t);
	if (rw->result == R_LINK || rw->result == R_STAT || rw->result == R_STATX ||
	    rw->result == R_XLIST)
		answer_at_entry(t, rw);         /* skipped when answered: no exit stop */
}

/* ---- results: what the program is told, not what the kernel returned --------- */

#define EXE_LEN   128
static char exe_now[PID_SLOTS][EXE_LEN];          /* by pid, the program each process runs */
static char exe_next[PID_SLOTS][EXE_LEN];         /* noted at execve entry, valid once it succeeds */

void trace_exe_pending(pid_t pid, const char *guest_path)
{
	scpy(exe_next[PID_SLOT(pid)], EXE_LEN, guest_path);
}

void trace_exe_commit(pid_t pid)
{
	unsigned i = PID_SLOT(pid);

	memcpy(exe_now[i], exe_next[i], EXE_LEN);
}

void trace_exe_fork(pid_t parent, pid_t child)
{
	*ids_of(child) = *ids_of(parent);
	memcpy(exe_now[PID_SLOT(child)], exe_now[PID_SLOT(parent)], EXE_LEN);
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
		const char *exe = exe_now[PID_SLOT(who)];

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

/* ---- answers at entry.  An exit stop costs a whole stop, a syscall made by the tracer
 * a few microseconds: for readlinkat, newfstatat and fstat the tracer makes the call on
 * the host path itself, writes what the program is told, and the syscall is skipped.
 * When it cannot name the file (a relative path from a directory outside the prefix,
 * a link too long for its buffer), it answers nothing and the exit stop rewrites the
 * kernel's result as before. */

unsigned long trace_n_answered;

/* "/proc/<pid>/fd/<fd>", or "/proc/<pid>/cwd" for AT_FDCWD when `cwd_ok` */
static int fd_name(pid_t pid, int fd, int cwd_ok, char *out, size_t cap)
{
	char dig[12];
	size_t i = 6;
	int nd = 0;

	if (cap < 48 || (fd < 0 && !(fd == -100 && cwd_ok)))
		return 0;
	memcpy(out, "/proc/", 6);
	do { dig[nd++] = (char) ('0' + pid % 10); pid /= 10; } while (pid);
	while (nd)
		out[i++] = dig[--nd];
	if (fd == -100) {
		scpy(out + i, cap - i, "/cwd");
		return 1;
	}
	memcpy(out + i, "/fd/", 4);
	i += 4;
	do { dig[nd++] = (char) ('0' + fd % 10); fd /= 10; } while (fd);
	while (nd)
		out[i++] = dig[--nd];
	out[i] = 0;
	return 1;
}

/* The path in register 1 as the tracer can name it: an absolute host path (/proc/self
 * spelled with the pid) -> 1; an empty path with `empty_fd` set (AT_EMPTY_PATH): the
 * directory fd in register 0 -> 2; anything else (a relative path the walk left as it
 * was) -> 0. */
static int name_for_tracer(struct tracee *t, int empty_fd, char *out, size_t cap)
{
	char path[256];
	ssize_t k = mon_read_str(t, t->r.regs[1], path, sizeof path);

	if (k <= 0 || memchr(path, 0, (size_t) k) == NULL)
		return 0;
	if (path[0] == '/') {
		self_to_pid(path, out, cap, t->pid);
		return 1;
	}
	if (path[0] == '\0' && empty_fd)
		return fd_name(t->pid, (int) t->r.regs[0], 1, out, cap) ? 2 : 0;
	return 0;
}

/* readlinkat(dirfd, path, buf, size) */
static int answer_link(struct tracee *t)
{
	char host[300], b[512], g[512];
	u64 buf = t->r.regs[2];
	i64 size = (i64) t->r.regs[3];
	const char *text = b;
	pid_t who;
	ssize_t r;
	int m;

	if (name_for_tracer(t, 0, host, sizeof host) != 1)
		return 0;
	if (size <= 0) {
		mon_skip(t, -22);                        /* EINVAL, as the kernel says */
		return 1;
	}
	if ((who = proc_exe_pid(host, t->pid)) != 0 && exe_now[PID_SLOT(who)][0]) {
		text = exe_now[PID_SLOT(who)];
		m = (int) strlen(text);
	} else {
		r = sys_readlink(host, b, sizeof b - 1);
		if (r < 0) {
			mon_skip(t, r);
			return 1;
		}
		if (r >= (ssize_t) sizeof b - 1)
			return 0;                        /* maybe longer than read: let the kernel do it */
		b[r] = '\0';
		m = policy_unpath(b, g, sizeof g);
		if (m > 0) {
			text = g;
			m--;                             /* the link text has no NUL */
		} else {
			m = (int) r;
		}
	}
	if ((i64) m > size)
		m = (int) size;
	mon_skip(t, mon_write(t, buf, text, (size_t) m) == m ? m : -14);   /* EFAULT */
	return 1;
}

/* a link's size as the program will read its target (fix_link_size, from the tracer) */
static void link_size(const char *host, u64 *size)
{
	char link[256], guest[256];
	ssize_t n = sys_readlink(host, link, sizeof link - 1);

	if (n <= 0)
		return;
	link[n] = 0;
	*size = policy_unpath(link, guest, sizeof guest) != 0 ? (u64) strlen(guest) : (u64) n;
}

/* the owner at buf + off (uid, then gid): the real ids read as root */
static void map_owner(char *buf, unsigned off)
{
	unsigned id[2];

	memcpy(id, buf + off, sizeof id);
	id[0] = policy_map_uid(id[0]);
	id[1] = policy_map_gid(id[1]);
	memcpy(buf + off, id, sizeof id);
}

/* statx(dirfd, path, flags, mask, buf).  struct statx is 256 bytes: the owner at 20
 * and 24, the mode at 28 (16 bits), the size at 40. */
static int answer_statx(struct tracee *t, const struct row *rw)
{
	char host[300], stx[256];
	int flags = (int) t->r.regs[2], how, r;
	unsigned short mode;
	u64 size;

	how = name_for_tracer(t, flags & 0x1000, host, sizeof host);            /* AT_EMPTY_PATH */
	if (how == 0)
		return 0;
	/* an fd is named by its /proc link, which is followed; the sync type is kept */
	r = sys_statx(host, flags & (how == 2 ? 0x6000 : 0x6900), (unsigned) t->r.regs[3], stx);
	if (r == -2 && how == 2 && (int) t->r.regs[0] >= 0)
		r = -9;                                  /* no such fd: EBADF */
	if (r < 0) {
		mon_skip(t, r);
		return 1;
	}
	map_owner(stx, 20);
	if (n_copies) {
		u64 ino;
		unsigned ma, mi, n;

		memcpy(&ino, stx + 32, sizeof ino);
		memcpy(&ma, stx + 136, sizeof ma);
		memcpy(&mi, stx + 140, sizeof mi);
		n = cl_count(((u64) (ma & 0xfffff000u) << 32) | ((u64) (ma & 0xfffu) << 8) |
			     ((u64) (mi & 0xffffff00u) << 12) | (mi & 0xffu), ino);
		if (n)
			memcpy(stx + 16, &n, sizeof n);
	}
	memcpy(&mode, stx + 28, sizeof mode);
	if (how == 1 && (mode & 0xf000) == 0xa000) {
		memcpy(&size, stx + 40, sizeof size);
		link_size(host, &size);
		memcpy(stx + 40, &size, sizeof size);
	}
	mon_skip(t, mon_write(t, t->r.regs[rw->result_arg], stx, sizeof stx) == (ssize_t) sizeof stx ? 0 : -14);
	return 1;
}

/* newfstatat(dirfd, path, buf, flags) and fstat(fd, buf).  struct stat is 128 bytes:
 * the mode at 16, the owner at 24 and 28, the size at 48. */
static int answer_stat(struct tracee *t, const struct row *rw)
{
	char host[300], st[128];
	int at = t->r.regs[8] == 79, flags = at ? (int) t->r.regs[3] : 0, how, r;
	unsigned mode;
	u64 size;

	how = at ? name_for_tracer(t, flags & 0x1000, host, sizeof host)          /* AT_EMPTY_PATH */
		 : (fd_name(t->pid, (int) t->r.regs[0], 0, host, sizeof host) ? 2 : 0);
	if (how == 0)
		return 0;
	r = sys_fstatat(host, st, how == 2 ? 0 : flags & 0x900);   /* AT_SYMLINK_NOFOLLOW, AT_NO_AUTOMOUNT */
	if (r == -2 && how == 2 && (int) t->r.regs[0] >= 0)
		r = -9;                                  /* no such fd: EBADF, as fstat says */
	if (r < 0) {
		mon_skip(t, r);
		return 1;
	}
	map_owner(st, 24);
	if (n_copies) {
		u64 dev, ino;
		unsigned n;

		memcpy(&dev, st, sizeof dev);
		memcpy(&ino, st + 8, sizeof ino);
		if ((n = cl_count(dev, ino)) != 0)
			memcpy(st + 20, &n, sizeof n);
	}
	memcpy(&mode, st + 16, sizeof mode);
	if (how == 1 && (mode & 0xf000) == 0xa000) {     /* a link: its size is the target as the program reads it */
		memcpy(&size, st + 48, sizeof size);
		link_size(host, &size);
		memcpy(st + 48, &size, sizeof size);
	}
	mon_skip(t, mon_write(t, t->r.regs[rw->result_arg], st, sizeof st) == (ssize_t) sizeof st ? 0 : -14);
	return 1;
}

/* unlinkat at entry, its path mapped: a copy losing a name */
static void unlink_copy(struct tracee *t)
{
	char host[300];
	u64 st[18];

	if (name_for_tracer(t, 0, host, sizeof host) == 1 && sys_fstatat(host, st, 0x100) == 0)
		cl_drop(st[0], st[1]);
}

/* listxattr(dirfd, list, size) and llistxattr: the list with the SELinux label
 * dropped, a Debian without SELinux having none.  Answered here (the syscall is
 * not run), so the length it reports is the filtered one.  The path in x0 is
 * already mapped by rewrite_path. */
static int answer_xlist(struct tracee *t, const struct row *rw)
{
	char host[300], tmp[8192];
	ssize_t n, i = 0, m = 0;
	u64 size;

	if (rw->nr == 13) {                            /* flistxattr: the fd names the file */
		if (!fd_name(t->pid, (int) t->r.regs[0], 0, host, sizeof host))
			return 0;
	} else {
		ssize_t k = mon_read_str(t, t->r.regs[0], host, sizeof host - 1);

		if (k <= 0)
			return 0;
		host[k] = '\0';
		if (host[0] != '/')
			return 0;                      /* a relative name: leave it to the kernel */
	}
	n = (ssize_t) sc(rw->nr == 13 ? 11 : rw->nr, (i64) host, (i64) tmp, (i64) sizeof tmp, 0, 0, 0);
	if (n < 0) {
		mon_skip(t, n);
		return 1;
	}
	while (i < n) {                                /* entries are NUL-terminated */
		size_t len = strlen(tmp + i);

		if (strcmp(tmp + i, "security.selinux") != 0) {
			if (m != i)
				memmove(tmp + m, tmp + i, len + 1);
			m += (ssize_t) len + 1;
		}
		i += (ssize_t) len + 1;
	}
	size = t->r.regs[2];
	if (size == 0) {
		mon_skip(t, m);
		return 1;
	}
	if (m > (ssize_t) size) {
		mon_skip(t, -34);                      /* ERANGE */
		return 1;
	}
	mon_skip(t, mon_write(t, t->r.regs[1], tmp, (size_t) m) == m ? m : -14);
	return 1;
}

static void answer_at_entry(struct tracee *t, const struct row *rw)
{
	int done = 0;

	if (rw->result == R_LINK)
		done = answer_link(t);
	else if (rw->result == R_STAT)
		done = answer_stat(t, rw);
	else if (rw->result == R_STATX)
		done = answer_statx(t, rw);
	else if (rw->result == R_XLIST)
		done = answer_xlist(t, rw);
	if (done)
		trace_n_answered++;
}

void trace_exit(struct tracee *t, const struct row *rw)
{
	if (rw->special == SP_SOCK_OUT) {               /* an AF_UNIX path the kernel wrote back */
		sock_map(t, rw, 1);
		return;
	}
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
