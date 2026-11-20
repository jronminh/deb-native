/* apply.c -- the applier.  It has no `if (syscall == ...)`: the row says where
 * the path is or what to fake, the policy says what it becomes, the monitor
 * moves the bytes. */
#include "gate.h"
#include "apply.h"

unsigned long trace_n_rewritten;

/* the owner store, used by link_copy before its definition */
static void own_write(const char *host, int nofollow, unsigned u, unsigned g);
static int own_lookup(const char *host, int nofollow, u64 dev, u64 ino, i64 sec, i64 nsec,
		      unsigned *u, unsigned *g);
static void own_apply(char *buf, unsigned off, const char *host, int nofollow,
		      u64 dev, u64 ino, i64 sec, i64 nsec);
static void store_owner(struct tracee *t);
static void mark_if_new(struct tracee *t);
static void dac_fix(struct tracee *t);

/* The per-pid state is a full-pid hash, not an array indexed by pid & 65535:
 * this phone's pid_max is 4194304, so two live pids 65536 apart would share an
 * entry.  A block is 1024 states, allocated on demand; trace_gone recycles. */
#define PID_BUCKETS 4096u
#define PID_BLOCK   1024u
struct pid_block {
	struct pid_state s[PID_BLOCK];
	struct pid_block *next;
};
static struct pid_state *pid_bucket[PID_BUCKETS];
static struct pid_state *pid_free;
static struct pid_block *pid_blocks;
static struct pid_state *pid_last;
static pid_t pid_last_pid;
static struct ids seed_ids;
static struct pid_state pid_sink;                 /* only if mmap fails */
static unsigned long n_forked = 1;                /* /proc/stat processes */
static unsigned long pid_live;                    /* /proc/stat procs_running */

static unsigned pid_hash(pid_t p)
{
	return ((unsigned) p * 2654435761u) >> (32 - 12);   /* 4096 buckets */
}

static struct pid_state *pid_new(void)
{
	unsigned i;

	if (!pid_free) {
		struct pid_block *b = (struct pid_block *) sc(222, 0, (i64) sizeof *b, 3, 0x22, -1, 0);

		if ((i64) b < 0)
			return &pid_sink;
		b->next = pid_blocks;
		pid_blocks = b;
		for (i = 0; i < PID_BLOCK; i++) {
			b->s[i].next = pid_free;
			pid_free = &b->s[i];
		}
	}
	{
		struct pid_state *s = pid_free;

		pid_free = s->next;
		memset(s, 0, sizeof *s);
		s->ids = seed_ids;
		return s;
	}
}

struct pid_state *pid_state_get(pid_t pid, int create)
{
	struct pid_state *s;
	unsigned h;

	if (pid_last && pid_last_pid == pid)
		return pid_last;
	h = pid_hash(pid);
	for (s = pid_bucket[h]; s; s = s->next)
		if (s->pid == pid) {
			pid_last = s;
			pid_last_pid = pid;
			return s;
		}
	if (!create)
		return 0;
	s = pid_new();
	if (s == &pid_sink)
		return s;
	s->pid = pid;
	s->next = pid_bucket[h];
	pid_bucket[h] = s;
	pid_last = s;
	pid_last_pid = pid;
	pid_live++;
	return s;
}

static void pid_drop(pid_t pid)
{
	struct pid_state **pp, *s;
	unsigned h = pid_hash(pid);

	for (pp = &pid_bucket[h]; (s = *pp); pp = &s->next)
		if (s->pid == pid) {
			*pp = s->next;
			if (pid_last == s) {
				pid_last = 0;
				pid_last_pid = 0;
			}
			s->next = pid_free;
			pid_free = s;
			if (pid_live)
				pid_live--;
			return;
		}
}

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
	policy_sock_init();                              /* the shared socket-path table */
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

static void drop(int effect)
{
	if (effect & E_LINKS)
		policy_links_changed();
	if (effect & E_CWD)
		cwd_gen++;
}

void trace_stop(pid_t pid)
{
	struct pid_state *s = pid_state_get(pid, 0);

	if (s && s->pending) {                          /* the call is over: what it changed is visible now */
		drop(s->pending);
		s->pending = 0;
	}
}

void trace_gone(pid_t pid)
{
	trace_stop(pid);
	cwd_cache[(unsigned) pid % CWD_SLOTS].pid = 0;
	pid_drop(pid);
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

#ifdef DN_HOST_ADB
/* The ADB host's `shell` domain has no `sock_file create` on shell_data_file,
 * where the tree lives, so a pathname socket cannot be made there (bind answers
 * EACCES).  An abstract name needs no file and the same domain may use it.  The
 * name is "\0dn-" and 64 bits of a hash over the prefix root and the resolved
 * host path, so bind and connect agree across sessions; the table keeps the
 * guest path an answer (getsockname, getpeername, accept) must show. */
#define SOCK_ABS_MAX   108
#define SOCK_ABS_SLOTS 128
struct sock_abs {
	u64 key;
	char guest[SOCK_ABS_MAX];
};
static struct sock_abs sock_abs[SOCK_ABS_SLOTS];
static unsigned sock_abs_next;
static u64 sock_abs_salt;
static int sock_abs_ready;

static u64 sock_fnv(const char *s)
{
	u64 h = 1469598103934665603ULL;

	while (*s) {
		h ^= (unsigned char) *s++;
		h *= 1099511628211ULL;
	}
	return h;
}

static u64 sock_key(const char *host)
{
	if (!sock_abs_ready) {
		const char *r = policy_root();

		sock_abs_salt = r ? sock_fnv(r) : 0;
		if (!sock_abs_salt)
			sock_abs_salt = 0x9e3779b97f4a7c15ULL;
		sock_abs_ready = 1;
	}
	return sock_fnv(host) ^ sock_abs_salt;
}

static void sock_remember(u64 key, const char *guest)
{
	size_t n = strlen(guest);
	unsigned i;

	if (n >= SOCK_ABS_MAX)
		return;
	for (i = 0; i < SOCK_ABS_SLOTS; i++)
		if (sock_abs[i].key == key)
			break;
	if (i == SOCK_ABS_SLOTS)
		i = sock_abs_next++ % SOCK_ABS_SLOTS;
	sock_abs[i].key = key;
	memcpy(sock_abs[i].guest, guest, n + 1);
}

static const char *sock_recall(u64 key)
{
	unsigned i;

	if (!key)
		return 0;
	for (i = 0; i < SOCK_ABS_SLOTS; i++)
		if (sock_abs[i].key == key)
			return sock_abs[i].guest;
	return 0;
}

/* A pathname bind would first create the socket in its parent directory: if the
 * real uid cannot write there, the abstract name would lie. */
static int sock_parent_ok(const char *host, int mode)
{
	char dir[420];
	size_t n = strlen(host);
	int i;

	if (n + 1 > sizeof dir)
		return 0;
	memcpy(dir, host, n + 1);
	for (i = (int) n - 1; i > 0 && dir[i] != '/'; i--)
		;
	if (i <= 0)
		return 0;
	dir[i] = '\0';
	return sc(48, -100, (i64) dir, mode, 0, 0, 0) == 0;
}

/* The abstract socket has no file, so stat, chmod and unlink on its path would
 * answer ENOENT.  A regular placeholder at the host path gives them the file
 * they expect; stat shows a regular file, not S_IFSOCK.  A real socket or a
 * symlink there is left alone: the path already means one. */
static void sock_mark_owner(struct tracee *t, const char *host);

static int sock_placeholder(struct tracee *t, const char *host)
{
	char st[256];
	unsigned mode = 0;
	int fd;

	if (sys_fstatat(host, st, 0x100) == 0) {          /* AT_SYMLINK_NOFOLLOW */
		mode = *(unsigned *) (st + 16);          /* st_mode on aarch64 */
		if ((mode & 0170000) == 0140000 ||        /* S_IFSOCK */
		    (mode & 0170000) == 0120000)          /* S_IFLNK */
			return 0;
	}
	fd = (int) sc(56, -100, (i64) host, 0x20241, 0600, 0, 0); /* W|CREAT|TRUNC|NOFOLLOW */
	if (fd < 0)
		return 1;                                /* the abstract name still works */
	sc(57, fd, 0, 0, 0, 0, 0);
	if (t)
		sock_mark_owner(t, host);
	return 1;
}

/* The abstract sun_path (leading NUL, then "dn-" and 16 hex digits): 20 bytes. */
static int sock_abs_name(const char *host, const char *guest, char *path)
{
	static const char hex[] = "0123456789abcdef";
	u64 key = sock_key(host);
	int i;

	path[0] = '\0';
	path[1] = 'd';
	path[2] = 'n';
	path[3] = '-';
	for (i = 0; i < 16; i++)
		path[4 + i] = hex[(key >> (60 - 4 * i)) & 15];
	path[20] = '\0';
	sock_remember(key, guest);
	return 20;
}

static int sock_abs_key(const char *p, u64 *key)
{
	u64 k = 0;
	int i;

	if (p[0] != '\0' || p[1] != 'd' || p[2] != 'n' || p[3] != '-')
		return 0;
	for (i = 0; i < 16; i++) {
		char c = p[4 + i];
		unsigned v;

		if (c >= '0' && c <= '9')
			v = (unsigned) (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (unsigned) (c - 'a' + 10);
		else
			return 0;
		k = (k << 4) | v;
	}
	*key = k;
	return 1;
}
#endif

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
	if (!out) {
		char host[420];
		unsigned nl;

		if (sa.path[0] != '/' || policy_path_nf(sa.path, host, sizeof host, 0) <= 0)
			return;                        /* already a host path */
		if (rw->nr == 200 && policy_row_ro(sa.path)) {   /* bind: a write to a `ro` row */
			mon_skip(t, -30);
			return;
		}
#ifdef DN_HOST_ADB
		{
			const char *root = policy_root();
			size_t rl = policy_root_len();

			/* Only the tree's own paths: a guest path that reaches a bind's
			 * host directory keeps a real pathname (its host may allow one). */
			if (root && rl && strncmp(host, root, rl) == 0 &&
			    (host[rl] == '/' || host[rl] == '\0') &&
			    sock_parent_ok(host, rw->nr == 200 ? 3 : 1) &&
			    (rw->nr != 200 || sock_placeholder(t, host))) {
				char ab[SOCK_ABS_MAX];

				nl = (unsigned) sock_abs_name(host, sa.path, ab);
				policy_sock_add(host);
				policy_sock_add(sa.path);
				if (mon_write(t, nb + 2, ab, nl) != (ssize_t) nl)
					return;
				nl += 2;
				if (mlp)
					mon_write(t, msg + 8, &nl, 4);
				else
					mon_set_arg(t, rw->path_arg2, nl);
				return;
			}
		}
#endif
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
#ifdef DN_HOST_ADB
		if (sa.path[0] == '\0') {              /* our abstract name: show the guest path */
			u64 key;
			const char *g;
			unsigned nl;

			if (!sock_abs_key(sa.path, &key) || (g = sock_recall(key)) == 0)
				return;
			if (strlen(g) + 1 > sizeof sa.path)
				return;
			if (mon_write(t, nb + 2, g, strlen(g) + 1) != (ssize_t) strlen(g) + 1)
				return;
			nl = 2 + (unsigned) strlen(g) + 1;
			if (mlp)
				mon_write(t, msg + 8, &nl, 4);
			else
				mon_write(t, lp, &nl, 4);
			return;
		}
#endif
		{
			char guest[120];
			int m = policy_unpath(sa.path, guest, sizeof guest);
			unsigned nl;

			if (m == 0)                    /* not under a row: leave it */
				return;
			if (mon_write(t, nb + 2, guest, strlen(guest) + 1) != (ssize_t) strlen(guest) + 1)
				return;
			nl = 2 + (unsigned) strlen(guest) + 1;
			if (mlp)
				mon_write(t, msg + 8, &nl, 4);
			else
				mon_write(t, lp, &nl, 4);  /* the length is a pointer on the way out */
		}
	}
}

static struct grps *seed_groups;

static struct pid_state *state_of(pid_t pid)
{
	return pid_state_get(pid, 1);
}

static const char *exe_of(pid_t pid)
{
	struct pid_state *s = pid_state_get(pid, 0);

	return s ? s->exe_now : "";
}

static struct grps *grp_new(unsigned n)
{
	size_t sz = sizeof(struct grps) + (size_t) n * sizeof(unsigned);
	struct grps *g = (struct grps *) sc(222, 0, (i64) sz, 3, 0x22, -1, 0);  /* mmap RW, PRIVATE|ANON */
	i64 p = (i64) g;

	if (p < 0)
		return 0;
	g->n = n;
	return g;
}

static struct grps *grp_of(pid_t pid)
{
	struct grps *g = state_of(pid)->groups;

	return g ? g : seed_groups;
}

static struct ids *ids_of(pid_t pid) { return &state_of(pid)->ids; }

#ifdef DN_HOST_ADB
/* The placeholder is the tracer's file: mark it with the tracee's ids, as a
 * create by that process would be marked, so stat and DAC see one owner. */
static void sock_mark_owner(struct tracee *t, const char *host)
{
	struct ids *id = ids_of(t->pid);

	own_write(host, 0, id->eu, id->eg);
}
#endif

/* capget/capset: the same virtual Cap* sets /proc/<pid>/status shows.  Root's
 * sets are full (the kernel's stay 0); everyone else has none.  The kernel's
 * own answer would be the app's, not the tree's view. */
static void answer_capget(struct tracee *t)
{
	struct { unsigned version; int pid; } h;
	unsigned d[6];

	if (mon_read(t, t->r.regs[0], &h, sizeof h) != sizeof h)
		return;
	if (h.version != 0x20080522)                     /* _LINUX_CAPABILITY_VERSION_3 */
		return;
	if (h.pid != 0 && h.pid != t->pid)
		return;                                  /* another pid: the kernel's */
	if (ids_of(t->pid)->eu == 0) {
		unsigned full[6] = { 0xffffffff, 0xffffffff, 0, 0x1ff, 0x1ff, 0 };

		memcpy(d, full, sizeof d);
	} else {
		memset(d, 0, sizeof d);
	}
	if (mon_write(t, t->r.regs[1], d, sizeof d) != sizeof d)
		return;
	mon_skip(t, 0);
}

static void answer_capset(struct tracee *t)
{
	struct { unsigned version; int pid; } h;

	if (mon_read(t, t->r.regs[0], &h, sizeof h) != sizeof h)
		return;
	if (h.version != 0x20080522 || (h.pid != 0 && h.pid != t->pid))
		return;
	mon_skip(t, ids_of(t->pid)->eu == 0 ? 0 : -1);   /* root's sets are fixed; EPERM otherwise */
}

/* the setuid-bit gate's pending euid, applied on the exec event */

void trace_identity_init(unsigned uid, unsigned gid)
{
	struct grps *g = 0;
	i64 k;

	if (uid == 0) {
		g = grp_new(1);
		if (g)
			g->g[0] = 0;
	} else {
		k = sc(158, 0, 0, 0, 0, 0, 0);            /* the app's real groups: count first */
		if (k >= 0 && k <= 65536) {
			g = grp_new((unsigned) k);
			if (g && k > 0 && sc(158, k, (i64) g->g, 0, 0, 0, 0) != k)
				g = 0;
		}
		if (!g) {
			g = grp_new(1);                   /* cannot ask: the primary group */
			if (g)
				g->g[0] = (unsigned) gid;
		}
	}
	seed_groups = g;
	seed_ids.ru = seed_ids.eu = seed_ids.su = uid;
	seed_ids.rg = seed_ids.eg = seed_ids.sg = gid;
	pid_free = 0;
	pid_blocks = 0;
	pid_last = 0;
	pid_last_pid = 0;
	memset(pid_bucket, 0, sizeof pid_bucket);
}

void trace_euid_next(pid_t pid, unsigned euid)
{
	struct pid_state *s = state_of(pid);

	s->euid_next = euid;
	s->euid_has_next = 1;
}

void trace_ids_of(pid_t pid, unsigned out[6])
{
	struct ids *d = ids_of(pid);

	out[0] = d->ru;
	out[1] = d->eu;
	out[2] = d->su;
	out[3] = d->rg;
	out[4] = d->eg;
	out[5] = d->sg;
}

int trace_stored_owner(const char *host, unsigned *u, unsigned *g)
{
	return policy_own_read(host, 0, u, g);
}

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

/* Changing ids is allowed for root, or for a non-root process to a target that
 * is already one of its real/effective/saved ids (POSIX setuid(2)).  A setuid
 * program that drops to a user keeps the saved root id, so it can return to 0;
 * apt's sandbox methods rely on this. */
static int id_is(unsigned v, unsigned a, unsigned b, unsigned c)
{
	return v == a || v == b || v == c;
}

static int setid_allowed(struct ids *d, unsigned nr, long a, long b, long c)
{
	if (d->eu == 0)
		return 1;
	switch (nr) {
	case 146:                                        /* setuid */
		return a == -1 || a == (long) d->ru || a == (long) d->su;
	case 145:                                        /* setreuid */
		return (a == -1 || a == (long) d->ru || a == (long) d->su) &&
		       (b == -1 || b == (long) d->ru || b == (long) d->su);
	case 147:                                        /* setresuid */
		return (a == -1 || id_is((unsigned) a, d->ru, d->eu, d->su)) &&
		       (b == -1 || id_is((unsigned) b, d->ru, d->eu, d->su)) &&
		       (c == -1 || id_is((unsigned) c, d->ru, d->eu, d->su));
	case 144:                                        /* setgid */
		return a == -1 || a == (long) d->rg || a == (long) d->sg;
	case 143:                                        /* setregid */
		return (a == -1 || a == (long) d->rg || a == (long) d->sg) &&
		       (b == -1 || b == (long) d->rg || b == (long) d->sg);
	case 149:                                        /* setresgid */
		return (a == -1 || id_is((unsigned) a, d->rg, d->eg, d->sg)) &&
		       (b == -1 || id_is((unsigned) b, d->rg, d->eg, d->sg)) &&
		       (c == -1 || id_is((unsigned) c, d->rg, d->eg, d->sg));
	case 151:                                        /* setfsuid */
		return a == -1 || id_is((unsigned) a, d->ru, d->eu, d->su);
	case 152:                                        /* setfsgid */
		return a == -1 || id_is((unsigned) a, d->rg, d->eg, d->sg);
	}
	return 0;
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

		if (!setid_allowed(d, (unsigned) nr, a, b, c)) {
			mon_skip(t, -1);                 /* EPERM */
			break;
		}
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
	case F_GROUPS: {                                /* getgroups(size, list) */
		struct grps *g = grp_of(t->pid);
		unsigned size = (unsigned) t->r.regs[0];

		if (size == 0) {
			mon_skip(t, g->n);
			break;
		}
		if (size < g->n) {
			mon_skip(t, -22);               /* EINVAL, as the kernel */
			break;
		}
		mon_write(t, t->r.regs[1], g->g, (size_t) g->n * sizeof(unsigned));
		mon_skip(t, g->n);
		break;
	}
	case F_ZERO: {                                  /* setgroups: root only, remembered */
		struct grps *g;
		unsigned size = (unsigned) t->r.regs[0];

		if (ids_of(t->pid)->eu != 0) {
			mon_skip(t, -1);                 /* EPERM */
			break;
		}
		if (size > 65536) {
			mon_skip(t, -22);                /* EINVAL, the kernel's NGROUPS_MAX */
			break;
		}
		g = grp_new(size);
		if (!g) {
			mon_skip(t, -12);                /* ENOMEM */
			break;
		}
		if (size > 0 && mon_read(t, t->r.regs[1], g->g, (size_t) size * sizeof(unsigned)) !=
				(ssize_t) (size * sizeof(unsigned))) {
			mon_skip(t, -14);                /* EFAULT */
			break;
		}
		state_of(t->pid)->groups = g;
		mon_skip(t, 0);
		break;
	}
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
	if (r == 0) {
		unsigned u, g;

		if (policy_own_read(old, 0, &u, &g))             /* the copy carries the stored owner */
			own_write(new, 0, u, g);
	}
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

/* ---- /proc/<pid>/status: the kernel's own, with the runtime's traces gone ----
 * TracerPid is dn-init and Seccomp is 2; a plain process has neither.  The file
 * is per-process and changes, so it is generated at open into a file under
 * var/lib/deb-native and the open is pointed there. ---------------------------- */
static int uitoa(char *p, unsigned v)
{
	char d[12];
	int j = 0, i = 0;

	do {
		d[j++] = (char) ('0' + v % 10);
		v /= 10;
	} while (v);
	while (j)
		p[i++] = d[--j];
	return i;
}

/* write a synthetic file under var/lib/deb-native and set `out` to its host path
 * (a memfd cannot be handed to the child across the ptrace boundary) */
static int put_synth(const char *name, const char *buf, size_t n, char *out, size_t cap)
{
	const char *root = policy_root();
	char fp[600];
	int fd;

	if (!root || policy_root_len() == 0)
		return 0;
	if (cat2(fp, sizeof fp, root, "/var/lib/deb-native/") < 0 || cat2(fp, sizeof fp, fp, name) < 0)
		return 0;
	fd = (int) sc(56, -100, (i64) fp, 0101 | 01000, 0600, 0, 0);   /* O_WRONLY|O_CREAT|O_TRUNC */
	if (fd < 0)
		return 0;
	sys_write(fd, buf, n);
	sys_close(fd);
	return scpy(out, cap, fp) < 0 ? 0 : 1;
}

/* "Uid:\t<r>\t<e>\t<s>\t<fs>\n" (or Gid), the four fields fake root answers: the
 * process's own ids, inherited by children.  The fourth is the effective id -- the
 * tracer keeps no separate fs id (setfsuid returns the effective one). */
static int id_line(char *b, const char *pfx, const struct ids *d, int group)
{
	unsigned r = group ? d->rg : d->ru, e = group ? d->eg : d->eu, s = group ? d->sg : d->su;
	int m = scpy(b, 64, pfx);

	m += uitoa(b + m, r);
	b[m++] = '\t';
	m += uitoa(b + m, e);
	b[m++] = '\t';
	m += uitoa(b + m, s);
	b[m++] = '\t';
	m += uitoa(b + m, e);
	b[m++] = '\n';
	return m;
}

/* "/proc/uptime": the boot time is real (CLOCK_BOOTTIME); the idle field, which
 * the kernel would give too, is approximated by the boot time (the phone is
 * mostly idle and there is no syscall for the aggregate idle). */
static int synth_uptime(char *out, size_t cap)
{
	struct { i64 s, ns; } ts = { 0, 0 };
	unsigned long sec, hh;
	char buf[64];
	int m = 0;

	if (sc(113, 7 /* CLOCK_BOOTTIME */, (i64) &ts, 0, 0, 0, 0) != 0)
		return 0;
	sec = (unsigned long) ts.s;
	hh = (unsigned long) (ts.ns / 10000000);
	m += uitoa(buf + m, (unsigned) sec);
	buf[m++] = '.';
	buf[m++] = (char) ('0' + hh / 10);
	buf[m++] = (char) ('0' + hh % 10);
	buf[m++] = ' ';
	m += uitoa(buf + m, (unsigned) sec);
	buf[m++] = '.';
	buf[m++] = (char) ('0' + hh / 10);
	buf[m++] = (char) ('0' + hh % 10);
	buf[m++] = '\n';
	return put_synth("uptime", buf, (size_t) m, out, cap);
}

/* "/proc/loadavg": the three loads and the process count are real (sysinfo); the
 * running count and the last pid have no syscall, so the current process stands
 * in (a tool that waits on the load reads the first field). */
static int synth_loadavg(char *out, size_t cap)
{
	struct {
		i64 uptime;
		unsigned long loads[3];
		unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
		unsigned short procs, pad;
		unsigned int pad2;
		unsigned long totalhigh, freehigh;
		unsigned int mem_unit;
	} si;
	char buf[80];
	int m = 0, i;

	if (sc(179 /* sysinfo */, (i64) &si, 0, 0, 0, 0, 0) != 0)
		return 0;
	for (i = 0; i < 3; i++) {
		unsigned long ip = si.loads[i] >> 16, fp = ((si.loads[i] & 0xffff) * 100) >> 16;

		m += uitoa(buf + m, (unsigned) ip);
		buf[m++] = '.';
		buf[m++] = (char) ('0' + fp / 10);
		buf[m++] = (char) ('0' + fp % 10);
		buf[m++] = ' ';
	}
	m += uitoa(buf + m, 1);
	buf[m++] = '/';
	m += uitoa(buf + m, si.procs);
	buf[m++] = ' ';
	m += uitoa(buf + m, (unsigned) sys_getpid());
	buf[m++] = '\n';
	return put_synth("loadavg", buf, (size_t) m, out, cap);
}

static int synth_status(struct tracee *t, const char *guest, char *out, size_t cap)
{
	const char *p;
	pid_t pid = 0;
	char kp[64], raw[16384], buf[16384], *w;
	size_t rn = 0;
	ssize_t n;
	int fd, m, isreal, isroot;

	if (strncmp(guest, "/proc/", 6) != 0 || policy_root_len() == 0)
		return 0;
	p = guest + 6;
	if (strncmp(p, "self/status", 11) == 0 && p[11] == '\0') {
		pid = t->pid;
	} else {
		const char *s = strchr(p, '/');

		if (!s || strcmp(s, "/status") != 0)
			return 0;
		for (; p < s; p++) {
			if (*p < '0' || *p > '9')
				return 0;
			pid = pid * 10 + (pid_t) (*p - '0');
		}
		if (p != s || pid <= 0)
			return 0;
	}
	{
		unsigned e = ids_of(pid)->eu;

		isreal = e == policy_real_uid();
		isroot = e == 0;
	}
	m = scpy(kp, sizeof kp, "/proc/");              /* the kernel's /proc/<pid>/status */
	if (m < 0)
		return 0;
	m += uitoa(kp + m, (unsigned) pid);
	if (scpy(kp + m, sizeof kp - m, "/status") < 0)
		return 0;
	fd = sys_open(kp, 0);
	if (fd < 0)
		return 0;
	n = sys_read(fd, raw, sizeof raw - 1);
	sys_close(fd);
	if (n <= 0)
		return 0;
	raw[n] = '\0';
	w = buf;
	for (p = raw; *p; ) {                           /* TracerPid: 0; drop Seccomp lines */
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t) (e - p + 1) : strlen(p);

		if (strncmp(p, "TracerPid:", 10) == 0) {
			const char *s = "TracerPid:\t0\n";

			if (rn + strlen(s) >= sizeof buf)
				break;
			memcpy(w + rn, s, strlen(s));
			rn += strlen(s);
		} else if (strncmp(p, "State:", 6) == 0) {
			const char *s = "State:\tR (running)\n";   /* not "t (tracing stop)" */

			if (rn + strlen(s) >= sizeof buf)
				break;
			memcpy(w + rn, s, strlen(s));
			rn += strlen(s);
		} else if (strncmp(p, "Name:", 5) == 0) {
			const char *b = exe_of(pid), *q, *sl = NULL;
			int l;

			for (q = b; *q; q++)
				if (*q == '/')
					sl = q;
			b = sl ? sl + 1 : b;
			if (rn + 8 + strlen(b) + 1 >= sizeof buf)
				break;
			memcpy(w + rn, "Name:\t", 6);
			rn += 6;
			l = (int) strlen(b);
			if (l > 15)
				l = 15;                        /* the kernel truncates comm to 15 */
			memcpy(w + rn, b, (size_t) l);
			rn += (size_t) l;
			w[rn++] = '\n';
		} else if (strncmp(p, "Uid:", 4) == 0 || strncmp(p, "Gid:", 4) == 0) {
			char l[64];                    /* the fake ids in root, the real in user */
			int q = isreal ? -1 :
				id_line(l, p[0] == 'U' ? "Uid:\t" : "Gid:\t", ids_of(pid), p[0] == 'G');

			if (q < 0) {
				if (rn + len >= sizeof buf)
					break;
				memcpy(w + rn, p, len);
				rn += len;
			} else {
				if (rn + (size_t) q >= sizeof buf)
					break;
				memcpy(w + rn, l, (size_t) q);
				rn += (size_t) q;
			}
		} else if (strncmp(p, "CapInh:", 7) == 0 || strncmp(p, "CapAmb:", 7) == 0 ||
			   strncmp(p, "CapPrm:", 7) == 0 || strncmp(p, "CapEff:", 7) == 0 ||
			   strncmp(p, "CapBnd:", 7) == 0) {
			const char *s;

			if (isreal) {                            /* the kernel's own caps */
				if (rn + len >= sizeof buf)
					break;
				memcpy(w + rn, p, len);
				rn += len;
				goto nextline;
			}
			if (strncmp(p, "CapInh:", 7) == 0 || strncmp(p, "CapAmb:", 7) == 0)
				s = "0000000000000000\n";
			else
				s = isroot ? "000001ffffffffff\n" : "0000000000000000\n";
			if (rn + 7 + strlen(s) >= sizeof buf)
				break;
			memcpy(w + rn, p, 7);                    /* the field name as it is */
			rn += 7;
			memcpy(w + rn, s, strlen(s));
			rn += strlen(s);
		} else if (strncmp(p, "NoNewPrivs:", 11) == 0) {
			const char *s = "NoNewPrivs:\t0\n";   /* Android sets it; the gate already made root */

			if (rn + strlen(s) >= sizeof buf) break;
			memcpy(w + rn, s, strlen(s)); rn += strlen(s);
		} else if (strncmp(p, "Seccomp:", 8) == 0 || strncmp(p, "Seccomp_filters:", 16) == 0) {
			;
		} else {
			if (rn + len >= sizeof buf)
				break;
			memcpy(w + rn, p, len);
			rn += len;
		}
nextline:
		if (!e)
			break;
		p = e + 1;
	}
	w[rn] = '\0';
	{
		char nm[32];
		int l = scpy(nm, sizeof nm, "status.");

		if (l < 0)
			return 0;
		l += uitoa(nm + l, (unsigned) pid);
		nm[l] = '\0';
		return put_synth(nm, w, rn, out, cap);
	}
}

/* "/proc/<pid>/cgroup": Android's leaks the app's uid and pid in the path; a tree
 * in no cgroup of its own says nothing (a plain v2 root). */
static int synth_cgroup(char *out, size_t cap)
{
	return put_synth("cgroup", "0::/\n", 5, out, cap);
}

/* The pid in a "/proc/<self|pid>/<leaf>" path, or 0. */
static pid_t proc_pid_leaf(struct tracee *t, const char *guest, const char *leaf)
{
	const char *s, *sl;
	pid_t pid = 0;

	if (strncmp(guest, "/proc/", 6) != 0)
		return 0;
	s = guest + 6;
	sl = strchr(s, '/');
	if (!sl || strcmp(sl + 1, leaf) != 0)
		return 0;
	if (sl - s == 4 && strncmp(s, "self", 4) == 0)
		return t->pid;
	for (; s < sl; s++) {
		if (*s < '0' || *s > '9')
			return 0;
		pid = pid * 10 + (*s - '0');
	}
	return pid;
}

/* One /proc/<pid>/maps line into dst: the file-backed path (the last field) is
 * mapped host->guest; a name it cannot map, an anonymous mapping and a smaps
 * field line are copied as they are. */
static void maps_line(char *dst, size_t cap, size_t *rn, const char *line, size_t len)
{
	const char *p = line, *end = line + len, *name, *nl;
	int f;

	for (f = 0; f < 5 && p < end; f++) {            /* the five fields before the name */
		while (p < end && *p == ' ')
			p++;
		while (p < end && *p != ' ' && *p != '\n')
			p++;
	}
	while (p < end && *p == ' ')
		p++;
	name = p;
	nl = name;
	while (nl < end && *nl != '\n')
		nl++;
	if (nl == name || (name[0] != '/' && name[0] != '[')) {   /* no name: the line as it is */
		if (*rn + len < cap) {
			memcpy(dst + *rn, line, len);
			*rn += len;
		}
		return;
	}
	{
		size_t namelen = (size_t) (nl - name), dl = 0;
		char tmp[400];
		const char *oname = name;
		size_t olen;

		if (namelen >= 10 && memcmp(name + namelen - 10, " (deleted)", 10) == 0) {
			dl = 10;
			namelen -= 10;
		}
		olen = namelen;
		if (name[0] == '/' && namelen > 0 && namelen < sizeof tmp) {
			char g[400];

			memcpy(tmp, name, namelen);
			tmp[namelen] = '\0';
			if (policy_unpath(tmp, g, sizeof g) > 0)
				memcpy(tmp, g, strlen(g) + 1);
			olen = strlen(tmp);
			oname = tmp;
		}
		if (*rn + (size_t) (name - line) + olen + dl + 2 < cap) {
			memcpy(dst + *rn, line, (size_t) (name - line));
			*rn += (size_t) (name - line);
			memcpy(dst + *rn, oname, olen);
			*rn += olen;
			if (dl) {
				memcpy(dst + *rn, " (deleted)", 10);
				*rn += 10;
			}
			dst[(*rn)++] = '\n';
		}
	}
}

/* "/proc/<pid>/maps" and "/proc/<pid>/smaps": the kernel's own, with the file
 * paths mapped to the tree's (gdb and the JVM open the names they read here). */
static int synth_maps(struct tracee *t, const char *guest, const char *leaf, char *out, size_t cap)
{
	static char raw[1 << 20], buf[1 << 20];
	pid_t pid = proc_pid_leaf(t, guest, leaf);
	char kp[64], nm[40];
	int fin, m;
	ssize_t n;
	size_t tot = 0, i, rn = 0;

	if (pid <= 0)
		return 0;
	m = scpy(kp, sizeof kp, "/proc/");
	m += uitoa(kp + m, (unsigned) pid);
	kp[m++] = '/';
	if (scpy(kp + m, sizeof kp - m, leaf) < 0)
		return 0;
	fin = sys_open(kp, 0);
	if (fin < 0)
		return 0;
	while (tot < sizeof raw - 1) {
		n = sys_read(fin, raw + tot, sizeof raw - 1 - tot);
		if (n <= 0)
			break;
		tot += (size_t) n;
	}
	sys_close(fin);
	if (tot == 0)
		return 0;
	for (i = 0; i < tot; ) {
		const char *e = memchr(raw + i, '\n', tot - i);
		size_t ll = e ? (size_t) (e - (raw + i)) + 1 : tot - i;

		maps_line(buf, sizeof buf, &rn, raw + i, ll);
		i += ll;
	}
	m = scpy(nm, sizeof nm, leaf);
	nm[m++] = '.';
	m += uitoa(nm + m, (unsigned) pid);
	nm[m] = '\0';
	return put_synth(nm, buf, rn, out, cap);
}

/* "/proc/stat": Android denies the aggregate, so the cpu/intr/ctxt counters are
 * a placeholder; btime is the kernel's real boot time and processes/procs_running
 * come from the tracer's own fork and live-pid counts. */
static int synth_stat(char *out, size_t cap)
{
	static char b[4096];
	int n = 0, i, cpus = 8;
	char rb[64], tail[192];
	long ts[2] = { 0, 0 };
	char si[112];
	unsigned long btime = 0;
	int fd = sys_open("/sys/devices/system/cpu/online", 0);   /* the real cpu count */

	if (sc(113, 0, (i64) ts, 0, 0, 0, 0) == 0 &&        /* clock_gettime(REALTIME) */
	    sc(179, (i64) si, 0, 0, 0, 0, 0) == 0) {        /* sysinfo: uptime */
		long up = *(long *) si;

		if (ts[0] > up)
			btime = (unsigned long) (ts[0] - up);
	}

	if (fd >= 0) {
		ssize_t k = sys_read(fd, rb, sizeof rb - 1);

		sys_close(fd);
		if (k > 0) {
			const char *p = rb, *q;
			int v = 0;

			rb[k] = '\0';
			for (q = rb; *q; q++)
				if (*q == '-')
					p = q + 1;
			for (; *p >= '0' && *p <= '9'; p++)
				v = v * 10 + (*p - '0');
			if (v >= 0 && v < 32)
				cpus = v + 1;
		}
	}
	n = scpy(b, sizeof b, "cpu  0 0 0 0 0 0 0 0 0 0\n");
	for (i = 0; i < cpus && n > 0; i++) {
		char t[16];
		int m = scpy(t, sizeof t, "cpu");

		m += uitoa(t + m, (unsigned) i);
		t[m] = '\0';
		if (scpy(b + n, sizeof b - n, t) < 0)
			break;
		n += m;
		n += scpy(b + n, sizeof b - n, " 0 0 0 0 0 0 0 0 0 0\n");
	}
	{
		int tn = 0;

		tn = scpy(tail, sizeof tail, "intr 0\nctxt 0\nbtime ");
		tn += uitoa(tail + tn, (unsigned) btime);
		tn += scpy(tail + tn, sizeof tail - tn, "\nprocesses ");
		tn += uitoa(tail + tn, (unsigned) n_forked);
		tn += scpy(tail + tn, sizeof tail - tn, "\nprocs_running ");
		tn += uitoa(tail + tn, (unsigned) pid_live);
		tn += scpy(tail + tn, sizeof tail - tn,
			   "\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0\n");
		if (tn > 0 && n + tn < (int) sizeof b) {
			memcpy(b + n, tail, (size_t) tn);
			n += tn;
		}
	}
	return put_synth("stat", b, strlen(b), out, cap);
}

/* "/proc/vmstat": denied too; a fixed placeholder with a marker key so a reader
 * knows it is not the kernel's. */
static int synth_vmstat(char *out, size_t cap)
{
	static const char b[] =
		"nr_free_pages 0\nnr_zone_inactive_anon 0\nnr_zone_active_anon 0\n"
		"nr_zone_inactive_file 0\nnr_zone_active_file 0\n"
		"pgpgin 0\npgpgout 0\npswpin 0\npswpout 0\npgfault 0\npgmajfault 0\n"
		"dn_not_faked 1\n";

	return put_synth("vmstat", b, sizeof b - 1, out, cap);
}

/* the pid a "/proc/<pid>/<leaf>" (or "/proc/self/<leaf>") names; 0 if it is not that */
static pid_t proc_leaf_pid(const char *guest, const char *leaf, pid_t self)
{
	const char *p = guest;
	pid_t v = 0;

	if (strncmp(p, "/proc/", 6) != 0)
		return 0;
	p += 6;
	if (strncmp(p, "self/", 5) == 0 && strcmp(p + 5, leaf) == 0)
		return self;
	if (*p < '0' || *p > '9')
		return 0;
	for (; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	if (p[0] != '/' || strcmp(p + 1, leaf) != 0)
		return 0;
	return v;
}

/* Is this token a glibc loader path?  The gate runs the tree's own PT_INTERP, so
 * its spelling is the tree's (Debian, Ubuntu, Arch, ...); the name is shared. */
static int loader_token(const char *s, size_t l)
{
	static const char ld[] = "ld-linux-aarch64.so.1";
	size_t m = sizeof ld - 1;

	if (l < m || memcmp(s + l - m, ld, m) != 0)
		return 0;
	return l == m || s[l - m - 1] == '/';
}

/* dn-exec's own command line is the bootstrap seam; it names the program it
 * will map.  The guest sees the program and its arguments, not dn-exec. */
static int dn_exec_token(const char *s, size_t l)
{
	static const char n[] = "dn-exec";
	size_t m = sizeof n - 1;

	return l == m ? memcmp(s, n, m) == 0
		: (l > m && s[l - m - 1] == '/' && memcmp(s + l - m, n, m) == 0);
}

/* "/proc/<pid>/cmdline": the kernel's own, minus the loader the exec gate puts in
 * front of a glibc program -- the loader, --preload/--audit and their host
 * paths, and the program token.  What is left is the argv the program was run
 * with, its paths the tree's.  A program the gate did not rewrite (static,
 * foreign) already reads its own argv, so its cmdline is left as it is. */
static int synth_cmdline(struct tracee *t, const char *guest, char *out, size_t cap)
{
	pid_t pid = proc_leaf_pid(guest, "cmdline", t->pid);
	char kp[64], raw[8192], buf[8192], nm[40];
	const char *tok, *end, *argv0 = NULL, *args = NULL;
	size_t argv0l = 0, argsl = 0, rn = 0, ldl;
	ssize_t n;
	int fd, m;

	if (pid <= 0 || policy_root_len() == 0)
		return 0;
	m = scpy(kp, sizeof kp, "/proc/");
	if (m < 0)
		return 0;
	m += uitoa(kp + m, (unsigned) pid);
	if (scpy(kp + m, sizeof kp - m, "/cmdline") < 0)
		return 0;
	fd = sys_open(kp, 0);
	if (fd < 0)
		return 0;
	n = sys_read(fd, raw, sizeof raw);
	sys_close(fd);
	if (n <= 0)
		return 0;
	{
		const char *z0 = memchr(raw, 0, (size_t) n);

		ldl = z0 ? (size_t) (z0 - raw) : 0;
	}
	{
		int dn = dn_exec_token(raw, ldl);

		if (!dn && !loader_token(raw, ldl))
			return 0;                      /* not the gate's command line: leave it */
		if (dn) {
			const char *tok, *prog = NULL;
			size_t len;

			end = raw + n;
			tok = raw + ldl + 1;               /* skip the dn-exec token */
			if (tok > end) tok = end;
			for (; tok < end; ) {
				const char *z = memchr(tok, 0, (size_t) (end - tok));
				size_t tl = z ? (size_t) (z - tok) : (size_t) (end - tok);

				if (tl == 6 && memcmp(tok, "--root", 6) == 0) {
					if (!z) break;
					tok = z + 1;
					z = memchr(tok, 0, (size_t) (end - tok));
					if (!z) break;
					tok = z + 1;
					continue;
				}
				if (tl > 0 && tok[0] == '-') { tok += tl + 1; continue; }
				prog = tok;
				break;
			}
			if (!prog)
				return 0;
			len = (size_t) (end - prog);
			if (len >= sizeof buf)
				return 0;
			memcpy(buf, prog, len);
			rn = len;
			if (rn == 0 || buf[rn - 1] != '\0')
				buf[rn++] = '\0';
			m = scpy(nm, sizeof nm, "cmdline.");
			if (m < 0)
				return 0;
			m += uitoa(nm + m, (unsigned) pid);
			nm[m] = '\0';
			return put_synth(nm, buf, rn, out, cap);
		}
	}
	end = raw + n;
	for (tok = raw; tok < end; ) {
		const char *z = memchr(tok, 0, (size_t) (end - tok));
		size_t tl = z ? (size_t) (z - tok) : (size_t) (end - tok);

		if (tl == 7 && memcmp(tok, "--argv0", 7) == 0) {
			const char *q = tok + 8, *r;
			size_t al;

			if (q >= end)
				break;
			r = memchr(q, 0, (size_t) (end - q));
			al = r ? (size_t) (r - q) : (size_t) (end - q);
			argv0 = q;
			argv0l = al;
			if (r == NULL)
				break;                 /* argv0 is the last token: no program */
			q = r + 1;                     /* the program token, skipped */
			if (q >= end)
				break;
			r = memchr(q, 0, (size_t) (end - q));
			if (r == NULL)
				break;                 /* no arguments past the program */
			args = r + 1;
			argsl = (size_t) (end - args);
			break;
		}
		tok += tl + 1;
	}
	if (!argv0)
		return 0;
	if (argv0l + 1 + argsl + 1 > sizeof buf)
		return 0;
	memcpy(buf, argv0, argv0l);
	rn = argv0l;
	buf[rn++] = '\0';
	if (argsl > 0) {
		memcpy(buf + rn, args, argsl);
		rn += argsl;
	}
	if (buf[rn - 1] != '\0')
		buf[rn++] = '\0';
	m = scpy(nm, sizeof nm, "cmdline.");
	if (m < 0)
		return 0;
	m += uitoa(nm + m, (unsigned) pid);
	nm[m] = '\0';
	return put_synth(nm, buf, rn, out, cap);
}

/* "/proc/<pid>/comm": the program, not the runtime (the loader names the process
 * the kernel sees); a process the runtime does not know is left as it is. */
static int synth_comm(struct tracee *t, const char *guest, char *out, size_t cap)
{
	pid_t pid = proc_leaf_pid(guest, "comm", t->pid);
	const char *b, *q, *sl = NULL;
	char buf[24], nm[40];
	size_t l;
	int m;

	if (pid <= 0 || policy_root_len() == 0)
		return 0;
	b = exe_of(pid);
	if (!b[0])
		return 0;
	for (q = b; *q; q++)
		if (*q == '/')
			sl = q;
	b = sl ? sl + 1 : b;
	l = strlen(b);
	if (l > 15)
		l = 15;                                /* the kernel truncates comm to 15 */
	memcpy(buf, b, l);
	buf[l++] = '\n';
	m = scpy(nm, sizeof nm, "comm.");
	if (m < 0)
		return 0;
	m += uitoa(nm + m, (unsigned) pid);
	nm[m] = '\0';
	return put_synth(nm, buf, l, out, cap);
}

/* "/proc/<pid>/stat": the kernel names the process after the loader (the gate runs
 * glibc through it); replace the comm between the parentheses with the program,
 * as /proc/<pid>/comm and status already do. */
static int synth_stat_pid(struct tracee *t, const char *guest, char *out, size_t cap)
{
	pid_t pid = proc_leaf_pid(guest, "stat", t->pid);
	char b[1024], ob[1200], nm[40];
	const char *e = exe_of(pid), *q, *sl = NULL, *r = NULL;
	ssize_t k;
	int fd, m = 0, l;

	if (pid <= 0 || !e[0])
		return 0;
	fd = sys_open(guest, 0);
	if (fd < 0)
		return 0;
	k = sys_read(fd, b, sizeof b - 1);
	sys_close(fd);
	if (k <= 0)
		return 0;
	b[k] = '\0';
	q = strchr(b, '(');
	for (r = b; *r; r++)
		if (*r == ')')
			break;
	if (!q || !*r || r < q)
		return 0;
	for (e = exe_of(pid); *e; e++)
		if (*e == '/')
			sl = e;
	e = sl ? sl + 1 : exe_of(pid);
	l = (int) strlen(e);
	if (l > 15)
		l = 15;
	memcpy(ob, b, (size_t) (q - b) + 1);
	m = (int) (q - b) + 1;
	memcpy(ob + m, e, (size_t) l);
	m += l;
	m += (int) scpy(ob + m, sizeof ob - (size_t) m, r);
	m = scpy(nm, sizeof nm, "stat.");
	m += uitoa(nm + m, (unsigned) pid);
	nm[m] = '\0';
	return put_synth(nm, ob, (size_t) (int) strlen(ob), out, cap);
}

/* A read-only open the tracer answers with a generated file: /proc/<pid>/status,
 * /proc/<pid>/cmdline, /proc/<pid>/comm, /proc/uptime, /proc/loadavg,
 * /proc/<pid>/cgroup, /proc/<pid>/maps, smaps, /proc/stat, /proc/vmstat.  1 if
 * `out` is the host path to open instead. */
static int synth_open(struct tracee *t, const char *guest, char *out, size_t cap)
{
	size_t gl = strlen(guest);

	if (strcmp(guest, "/proc/stat") == 0)
		return synth_stat(out, cap);
	if (strcmp(guest, "/proc/vmstat") == 0)
		return synth_vmstat(out, cap);
	if (strcmp(guest, "/proc/sys/kernel/pid_max") == 0)              /* the denied /proc/sys: a
									  * fixed Debian default, not the
									  * phone's live value */
		return put_synth("pid_max", "4194304\n", 8, out, cap);
	if (strcmp(guest, "/proc/sys/vm/max_map_count") == 0)
		return put_synth("max_map_count", "65530\n", 6, out, cap);
	if (strcmp(guest, "/proc/sys/kernel/cap_last_cap") == 0)
		return put_synth("cap_last_cap", "40\n", 3, out, cap);
	if (strcmp(guest, "/proc/uptime") == 0)
		return synth_uptime(out, cap);
	if (strcmp(guest, "/proc/loadavg") == 0)
		return synth_loadavg(out, cap);
	if (gl > 7 && strncmp(guest, "/proc/", 6) == 0 && strcmp(guest + gl - 7, "/cgroup") == 0)
		return synth_cgroup(out, cap);
	if (gl > 5 && strncmp(guest, "/proc/", 6) == 0 && strcmp(guest + gl - 5, "/maps") == 0)
		return synth_maps(t, guest, "maps", out, cap);
	if (gl > 6 && strncmp(guest, "/proc/", 6) == 0 && strcmp(guest + gl - 6, "/smaps") == 0)
		return synth_maps(t, guest, "smaps", out, cap);
	if (proc_leaf_pid(guest, "cmdline", t->pid) > 0)
		return synth_cmdline(t, guest, out, cap);
	if (proc_leaf_pid(guest, "comm", t->pid) > 0)
		return synth_comm(t, guest, out, cap);
	if (proc_leaf_pid(guest, "stat", t->pid) > 0)
		return synth_stat_pid(t, guest, out, cap);
	return synth_status(t, guest, out, cap);
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

/* access()/faccessat: the world's answer, not the kernel's.  The kernel always
 * sees the app as the owner, so it would allow what the world denies and deny
 * what the fake root's DAC override allows.  Follow the euid: root passes (X_OK
 * still needs one execute bit on a regular file), anyone else is checked
 * against the stored owner and the virtual group list.  Host rows and a name
 * this cannot resolve are left to the kernel. */
static int answer_access(struct tracee *t)
{
	char in[256], host[600], base[256];
	char st[144];
	u64 dev, ino;
	i64 sec, nsec;
	ssize_t k;
	unsigned m = (unsigned) t->r.regs[2], smode, u, g, eu, eg, need, have, shift;
	struct grps *grp;
	int n, nf;

	if (m & ~7u)
		return 0;                        /* bits the kernel will refuse */
	k = mon_read_str(t, t->r.regs[1], in, sizeof in - 1);
	if (k <= 0 || in[0] == '\0' || memchr(in, 0, (size_t) k) == NULL)
		return 0;                        /* "" is ENOENT, not the cwd */
	in[k] = '\0';
	if (policy_is_host(in))
		return 0;                        /* /proc, /sys, /dev, a bind: the kernel's */
	nf = policy_nofollow((unsigned) t->r.regs[8], 1, (const u64 *) t->r.regs);
	if (in[0] == '/')
		n = policy_path_nf(in, host, sizeof host, nf);
	else {
		if (!base_of(t, 1, base, sizeof base - 1))
			return 0;
		n = policy_path_at(base, in, host, sizeof host, nf);
	}
	if (n == 0)
		return 0;
	{
		const char *r = policy_root();
		size_t rl = policy_root_len();

		if (!rl || strncmp(host, r, rl) != 0 || (host[rl] != '/' && host[rl] != '\0'))
			return 0;                /* a bind or a host row: the kernel's */
	}
	if (sys_fstatat(host, st, nf ? 0x100 : 0) != 0)
		return 0;                        /* let the kernel give the error */

	memcpy(&dev, st, 8);
	memcpy(&ino, st + 8, 8);
	memcpy(&sec, st + 104, 8);
	memcpy(&nsec, st + 112, 8);
	if (!own_lookup(host, nf, dev, ino, sec, nsec, &u, &g))
		u = g = 0;                       /* unmarked: the implicit root's */
	memcpy(&smode, st + 16, 4);
	eu = ids_of(t->pid)->eu;
	eg = ids_of(t->pid)->eg;

	if (eu == 0) {
		if ((m & 1) && (smode & 0170000) != 0040000 && !(smode & 0111)) {
			mon_skip(t, -13);        /* EACCES: root still needs an x bit */
			return 1;
		}
		mon_skip(t, 0);
		return 1;
	}
	need = ((m & 4) ? 4u : 0) | ((m & 2) ? 2u : 0) | ((m & 1) ? 1u : 0);
	if (need == 0) {
		mon_skip(t, 0);                  /* F_OK: it exists */
		return 1;
	}
	if (eu == u) {
		shift = 6;
	} else {
		have = (eg == g);
		if (!have && (grp = grp_of(t->pid)) != 0)
			for (u = 0; u < grp->n; u++)
				if (grp->g[u] == g) {
					have = 1;
					break;
				}
		shift = have ? 3 : 0;
	}
	have = (smode >> shift) & 7;
	mon_skip(t, (have & need) == need ? 0 : -13);
	return 1;
}

void trace_enter(struct tracee *t, const struct row *rw)
{
	if (rw->effect) {                               /* drop now, and again once the call is over */
		drop(rw->effect);
		state_of(t->pid)->pending |= (unsigned char) rw->effect;
	}
	if (rw->nr == 90) {                             /* capget: the virtual sets */
		answer_capget(t);
		return;
	}
	if (rw->nr == 91) {                             /* capset: fixed for root, EPERM */
		answer_capset(t);
		return;
	}
	if (rw->nr == 123) {                            /* sched_getaffinity: a stable 8 */
		unsigned char m[128];
		size_t n = (size_t) t->r.regs[1];
		pid_t q = (pid_t) t->r.regs[0];
		unsigned i;

		if (q != 0 && q != t->pid && pid_state_get(q, 0) == 0)
			return;
		if (n < 8) {                            /* the kernel's cpumask_size is 8 */
			mon_skip(t, -22);
			return;
		}
		if (n > sizeof m)
			n = sizeof m;
		memset(m, 0, n);
		for (i = 0; i < 8 && i / 8 < n; i++)
			m[i / 8] |= (unsigned char) (1u << (i % 8));
		if (mon_write(t, t->r.regs[2], m, n) == (ssize_t) n)
			mon_skip(t, 8);
		return;
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
	if (rw->nr == 56 && !(t->r.regs[2] & 0x643)) {  /* a read-only open: the synthetic /proc/<pid>/status */
		char in[256], h[600];
		ssize_t k = mon_read_str(t, t->r.regs[1], in, sizeof in - 1);

		if (k > 0 && (in[k] = '\0', 1) && synth_open(t, in, h, sizeof h)) {
			u64 at = mon_scratch(t, h, strlen(h) + 1);

			if (at != 0) {
				mon_set_arg(t, 1, at);
				return;
			}
		}
	}
	if (rw->nr == 167) {                             /* prctl: keep the tracer's access */
		int op = (int) t->r.regs[0];

		if (op == 39 ||                         /* PR_GET_NO_NEW_PRIVS: Android set it */
		    op == 4) {                          /* PR_SET_DUMPABLE: keep /proc/<pid>/mem usable */
			mon_skip(t, 0);
			return;
		}
		if (op == 23) {                         /* PR_CAPBSET_READ */
			int cap = (int) t->r.regs[1];

			mon_skip(t, cap < 0 || cap > 40 ? -22 :
				 ids_of(t->pid)->eu == 0 ? 1 : 0);
			return;
		}
		if (op == 24) {                         /* PR_CAPBSET_DROP */
			mon_skip(t, ids_of(t->pid)->eu == 0 ? 0 : -1);
			return;
		}
		return;                                  /* every other prctl runs as it is */
	}
	if (rw->nr == 54 || rw->nr == 55) {              /* chown: kept in user.dn.owner */
		store_owner(t);
		return;
	}
	if ((rw->nr == 48 || rw->nr == 439) && answer_access(t))
		return;                                  /* access: the virtual view */
	if (rw->fake != F_NONE) {
		/* Every fake is answered here: get*id from the state, set*id with the
		 * saved-id rules, groups from the virtual list (chown is earlier). */
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
	if (rw->nr == 56)
		dac_fix(t);                     /* a fake-root write: owner-write, restored at exit */
	if ((rw->nr == 56 && (t->r.regs[2] & 0x40) && !(t->r.regs[2] & 0x400000)) ||
	    rw->nr == 34 || rw->nr == 33)
		mark_if_new(t);                 /* a create: stop at exit and mark it */
	if (rw->special == SP_LINK)             /* linkat: the platform denies it, so it is a copy */
		link_by_copy(t);
	if (rw->nr == 35 && n_copies && !(t->r.regs[2] & 0x200))   /* unlinkat, not AT_REMOVEDIR */
		unlink_copy(t);
	if (rw->result == R_LINK || rw->result == R_STAT || rw->result == R_STATX ||
	    rw->result == R_XLIST)
		answer_at_entry(t, rw);         /* skipped when answered: no exit stop */
}

/* ---- results: what the program is told, not what the kernel returned --------- */

void trace_exe_pending(pid_t pid, const char *guest_path)
{
	scpy(state_of(pid)->exe_next, EXE_LEN, guest_path);
}

void trace_exe_commit(pid_t pid)
{
	struct pid_state *s = state_of(pid);

	memcpy(s->exe_now, s->exe_next, EXE_LEN);
	if (s->euid_has_next) {
		s->euid_has_next = 0;
		s->ids.eu = s->ids.su = s->euid_next;    /* a setuid exec: ruid stays as it was */
	}
}

void trace_exe_fork(pid_t parent, pid_t child)
{
	struct pid_state *p = pid_state_get(parent, 0);
	struct pid_state *c = state_of(child);

	if (!p)
		return;
	c->ids = p->ids;
	c->groups = p->groups;
	c->euid_has_next = 0;
	c->pending = 0;
	memcpy(c->exe_now, p->exe_now, EXE_LEN);
	n_forked++;
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
		const char *exe = exe_of(who);

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
	char host[300];
	unsigned id[2], own[2], nr = (unsigned) t->r.regs[8];
	u64 dev = 0, ino = 0;
	i64 sec = 0, nsec = 0;
	ssize_t k;

	if (t->r.regs[0] != 0 || mon_read(t, buf + off, id, sizeof id) != (ssize_t) sizeof id)
		return;
	k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
	if (k <= 0 || host[0] != '/')
		return;                                  /* the path, mapped at entry */
	host[k] = '\0';
	if (nr == 79) {                                  /* struct stat */
		if (mon_read(t, buf, &dev, 8) != 8 || mon_read(t, buf + 8, &ino, 8) != 8 ||
		    mon_read(t, buf + 104, &sec, 8) != 8 || mon_read(t, buf + 112, &nsec, 8) != 8)
			return;
	} else {                                         /* struct statx */
		unsigned ma = 0, mi = 0;

		if (mon_read(t, buf + 32, &ino, 8) != 8 || mon_read(t, buf + 136, &ma, 4) != 4 ||
		    mon_read(t, buf + 140, &mi, 4) != 4 || mon_read(t, buf + 96, &sec, 8) != 8 ||
		    mon_read(t, buf + 104, &nsec, 8) != 8)
			return;
		dev = ((u64) (ma & 0xfffff000u) << 32) | ((u64) (ma & 0xfffu) << 8) |
		      ((u64) (mi & 0xffffff00u) << 12) | (mi & 0xffu);
	}
	if (!own_lookup(host, 0, dev, ino, sec, nsec, &own[0], &own[1]))
		own[0] = own[1] = 0;             /* unmarked: the implicit root, both worlds */
	mon_write(t, buf + off, own, sizeof own);
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
	if ((who = proc_exe_pid(host, t->pid)) != 0 && exe_of(who)[0]) {
		text = exe_of(who);
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

/* ---- the owner store: chown is kept in user.dn.owner ("UID:GID"), and every
 * stat reads it back (the file's real owner is the app's).  The read and its
 * parse live in the law (law.c), shared with the shim and dn-exec. --------- */

#define OWN_ATTR "user.dn.owner"

/* Every chown is stored, 0:0 included: the only implicit owner is the real uid,
 * which the fake world (euid not the app's) maps to 0.  Storing 0:0 is what
 * lets a file chowned by root read as root for the real user too. */
static void own_write(const char *host, int nofollow, unsigned u, unsigned g)
{
	char b[32];
	int n = uitoa(b, u);

	b[n++] = ':';
	n += uitoa(b + n, g);
	sc(nofollow ? 6 : 5, (i64) host, (i64) OWN_ATTR, (i64) b, n, 0, 0);       /* setxattr */
}

/* ---- the owner cache: (dev, ino, ctime) -> the stored owner.  chown changes
 * ctime, so an entry cannot go stale; a chmod or a write only makes it miss
 * sooner.  Direct-mapped: a collision costs a getxattr, never a wrong answer. --- */
#define OCN 1024
struct ownc { u64 dev, ino; i64 sec, nsec; unsigned u, g; unsigned char present, valid; };
static struct ownc ocache[OCN];

static unsigned oc_hash(u64 dev, u64 ino)
{
	return (unsigned) ((dev ^ (dev >> 32) ^ ino ^ (ino >> 32)) & (OCN - 1));
}

static struct ownc *oc_find(u64 dev, u64 ino, i64 sec, i64 nsec)
{
	struct ownc *e = &ocache[oc_hash(dev, ino)];

	if (!e->valid || e->dev != dev || e->ino != ino || e->sec != sec || e->nsec != nsec)
		return NULL;
	return e;
}

/* the stored owner, from the cache or from the xattr; 1 if present */
static int own_lookup(const char *host, int nofollow, u64 dev, u64 ino, i64 sec, i64 nsec,
		      unsigned *u, unsigned *g)
{
	struct ownc *e = oc_find(dev, ino, sec, nsec);

	if (e) {
		if (!e->present)
			return 0;
		*u = e->u;
		*g = e->g;
		return 1;
	}
	e = &ocache[oc_hash(dev, ino)];
	e->dev = dev;
	e->ino = ino;
	e->sec = sec;
	e->nsec = nsec;
	e->valid = 1;
	if (policy_own_read(host, nofollow, &e->u, &e->g)) {
		e->present = 1;
		*u = e->u;
		*g = e->g;
		return 1;
	}
	e->present = 0;
	e->u = e->g = 0;
	return 0;
}

/* read the stored owner into a stat/statx buffer at off (uid, then gid).  A file
 * with no store is the implicit root's, in both worlds: a legacy file (or a
 * symlink, which takes no user xattr) reads the same to root and to the real
 * user, so the two worlds cannot disagree about it. */
static void own_apply(char *buf, unsigned off, const char *host, int nofollow,
		      u64 dev, u64 ino, i64 sec, i64 nsec)
{
	unsigned id[2];

	if (!own_lookup(host, nofollow, dev, ino, sec, nsec, &id[0], &id[1]))
		id[0] = id[1] = 0;
	memcpy(buf + off, id, sizeof id);
}

/* fchownat (54) and fchown (55): answer 0, as the fake root did, and remember the
 * owner.  A symlink takes no user xattr, so its chown stays answer-only. */
static void store_owner(struct tracee *t)
{
	char host[300];

	if (ids_of(t->pid)->eu != 0) {
		mon_skip(t, -1);                 /* EPERM: only root chowns, as on Debian */
		return;
	}
	unsigned u = (unsigned) t->r.regs[2], g = (unsigned) t->r.regs[3];
	unsigned cu = 0, cg = 0;
	int nofollow = 0;
	ssize_t k;

	if (t->r.regs[8] == 54) {
		nofollow = !!(t->r.regs[4] & 0x100);     /* chown -R uses it; a symlink takes none */
		rewrite_path(t, 1);
		k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
		if (k <= 0 || host[0] != '/')
			goto out;                        /* a relative name outside the tree */
		host[k] = '\0';
	} else if (!fd_name(t->pid, (int) t->r.regs[0], 0, host, sizeof host)) {
		goto out;
	}
	if (u != (unsigned) -1 || g != (unsigned) -1) {
		if (!policy_own_read(host, nofollow, &cu, &cg)) {
			cu = 0;
			cg = 0;
		}
		if (u != (unsigned) -1)
			cu = u;
		if (g != (unsigned) -1)
			cg = g;
		own_write(host, nofollow, cu, cg);
	}
out:
	mon_skip(t, 0);
}

/* statx(dirfd, path, flags, mask, buf).  struct statx is 256 bytes: the owner at 20
 * and 24, the mode at 28 (16 bits), the size at 40. */
static int answer_statx(struct tracee *t, const struct row *rw)
{
	char host[300], stx[256];
	int flags = (int) t->r.regs[2], how, r;
	unsigned short mode;
	u64 size, odev, oino;
	i64 osec, onsec;

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
	{
		unsigned ma, mi;

		memcpy(&oino, stx + 32, sizeof oino);
		memcpy(&ma, stx + 136, sizeof ma);
		memcpy(&mi, stx + 140, sizeof mi);
		odev = ((u64) (ma & 0xfffff000u) << 32) | ((u64) (ma & 0xfffu) << 8) |
		       ((u64) (mi & 0xffffff00u) << 12) | (mi & 0xffu);
		memcpy(&osec, stx + 96, sizeof osec);
		memcpy(&onsec, stx + 104, sizeof onsec);
	}
	own_apply(stx, 20, host, how == 1 && (flags & 0x100), odev, oino, osec, onsec);
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
	u64 size, odev, oino;
	i64 osec, onsec;

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
	memcpy(&odev, st, sizeof odev);
	memcpy(&oino, st + 8, sizeof oino);
	memcpy(&osec, st + 104, sizeof osec);
	memcpy(&onsec, st + 112, sizeof onsec);
	own_apply(st, 24, host, how == 1 && (flags & 0x100), odev, oino, osec, onsec);
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

/* The fake root has no CAP_DAC_OVERRIDE, so writing a file whose owner bits lack
 * write would fail where root succeeds (dpkg's 0440 .dpkg-new is the usual
 * case).  Add the owner write bit for the open, and put the mode back at the
 * exit stop: the fd keeps the access it was opened with.  Only write opens;
 * reads that root alone could do are rare. */
static void dac_fix(struct tracee *t)
{
	char host[300];
	u64 st[18];
	ssize_t k;
	unsigned flags = (unsigned) t->r.regs[2], mode, need = 0200;
	int nofollow = (flags & 0x8000) != 0;

	if (ids_of(t->pid)->eu != 0)               /* only the fake root */
		return;
	if ((flags & 3) == 0 && !(flags & 0x200))   /* O_RDONLY without O_TRUNC */
		return;
	k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
	if (k <= 0 || host[0] != '/')
		return;
	host[k] = '\0';
	if (sys_fstatat(host, st, nofollow ? 0x100 : 0) != 0)
		return;                          /* a create: the open may make it */
	mode = *(unsigned *) ((char *) st + 16);
	if ((mode & 0xf000) == 0xa000 && nofollow)
		return;                          /* O_NOFOLLOW on a symlink: ELOOP anyway */
	if ((mode & 0xf000) == 0x4000)     /* a directory: write also needs execute */
		need = 0300;
	if ((mode & need) == need)
		return;
	if (sc(53, -100, (i64) host, (i64) (mode | need), 0, 0, 0) != 0)
		return;                          /* fchmodat */
	t->dac_exit = 1;
	t->dac_mode = mode;
}

void trace_dac_restore(struct tracee *t)
{
	char host[300];
	ssize_t k;

	if (!t->dac_exit)
		return;
	t->dac_exit = 0;
	k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
	if (k <= 0 || host[0] != '/')
		return;
	host[k] = '\0';
	sc(53, -100, (i64) host, (i64) t->dac_mode, 0, 0, 0);
}

/* A create (openat with O_CREAT, mkdirat, mknodat): if the name does not exist,
 * the syscall will make it, so ask for the exit stop and mark the new file with
 * the creator's identity.  A pre-stat race is the same race the syscall has. */
static void mark_if_new(struct tracee *t)
{
	char host[300];
	u64 st[18];
	ssize_t k;
	int nofollow = 0;

	k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
	if (k <= 0 || host[0] != '/')
		return;
	host[k] = '\0';
	if (t->r.regs[8] == 56)
		nofollow = (t->r.regs[2] & 0x8000) != 0;      /* O_NOFOLLOW */
	if (sys_fstatat(host, st, nofollow ? 0x100 : 0) == -2)
		t->mark_exit = 1;
}

/* the create succeeded: the file exists now; store the creator's ids, so the
 * owner reads the same in the fake world and for the real user. */
void trace_mark_owner(struct tracee *t)
{
	char host[300];
	struct ids *d;
	ssize_t k;

	if (!t->mark_exit)
		return;
	t->mark_exit = 0;
	if ((i64) t->r.regs[0] < 0)                        /* the call failed */
		return;
	k = mon_read_str(t, t->r.regs[1], host, sizeof host - 1);
	if (k <= 0 || host[0] != '/')
		return;
	host[k] = '\0';
	d = ids_of(t->pid);
	own_write(host, 0, d->eu, d->eg);
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

/* /proc's getdents64: keep the entries whose numeric name is a traced pid, so
 * ps/top do not list Android's processes or the kernel's threads.  The fd must
 * be /proc itself; a per-pid subdirectory is left alone. */
static void filter_proc(struct tracee *t)
{
	struct dn_dirent {
		u64 ino;
		i64 off;
		unsigned short reclen;
		unsigned char type;
		char name[];
	};
	char link[64], path[256], buf[32768], out[32768];
	long n = (long) t->r.regs[0];
	long off = 0, w = 0;
	int l;
	ssize_t k;

	if (n <= 0 || n > (long) sizeof buf)
		return;
	l = scpy(link, sizeof link, "/proc/");
	l += uitoa(link + l, (unsigned) t->pid);
	l += scpy(link + l, sizeof link - (size_t) l, "/fd/");
	l += uitoa(link + l, (unsigned) t->entry_arg0);
	if (l <= 0 || l >= (int) sizeof link)
		return;
	k = sys_readlink(link, path, sizeof path - 1);
	if (k <= 0)
		return;
	path[k] = '\0';
	if (strcmp(path, "/proc") != 0 && strcmp(path, "/proc/") != 0)
		return;
	if (mon_read(t, t->r.regs[1], buf, (size_t) n) != n)
		return;
	while (off < n) {
		struct dn_dirent *d = (struct dn_dirent *) (buf + off);
		unsigned long pid = 0;
		int numeric = 1;
		char *q;

		if (d->reclen < 19 || (long) d->reclen > n - off)
			break;
		for (q = d->name; *q; q++) {
			if (*q < '0' || *q > '9') {
				numeric = 0;
				break;
			}
			pid = pid * 10 + (unsigned long) (*q - '0');
		}
		if (numeric && pid > 0 && pid_state_get((pid_t) pid, 0) == 0) {
			off += d->reclen;
			continue;
		}
		memcpy(out + w, d, d->reclen);
		w += d->reclen;
		off += d->reclen;
	}
	if (w != n) {
		mon_write(t, t->r.regs[1], out, (size_t) w);
		t->r.regs[0] = (u64) w;
		t->dirty = 1;
	}
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
	case R_DIRENT: filter_proc(t); break;
	default: break;
	}
}

/* The platform's filter killed this syscall (the tracee is at the SIGSYS stop,
 * its program counter already past the `svc`).  Set the registers so that, once
 * resumed, the program sees what the policy says.  A substitute is run by moving
 * the program counter back to the `svc` with the new number: it goes through our
 * filter like any other call, so its path is mapped too. */
#ifndef DN_HOST_ADB
void trace_denied(struct tracee *t)
{
	struct deny d;
	unsigned nr = (unsigned) t->r.regs[8];
	const struct row *rw = policy_row(nr);

	/* A call the platform kills never reaches the entry stop (seccomp takes the strictest
	 * answer, and TRAP outranks TRACE), so a faked call is answered here the same way. */
	if (rw && rw->fake != F_NONE) {
		fake_result(t, rw->fake);        /* the saved-id rules answer EPERM */
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
#endif
