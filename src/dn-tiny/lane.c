/* lane.c -- dn-lane.so, the shim of dn-tiny: the exec gate preloads it into every
 * glibc program it starts (ld.so --preload).  It does the tracer's path work inside
 * the program, for the calls that only read, so that they cost no stop:
 *
 *   realpath, canonicalize_file_name     resolved here, one cached readlink a component
 *   open, openat (read-only flags)       opened here, so the fd is the program's own
 *   stat, lstat, fstatat, fstat, statx   stat here, the owner read as root, a link's size
 *   access, faccessat (no flags)         checked here
 *   readlink, readlinkat (not /proc)     read here, a target inside the prefix as the guest's
 *   [l]getxattr, [l]listxattr            read here
 *   getcwd                               from a copy, dropped by chdir and by a rename
 *   getuid, geteuid, getresuid, ...      answered from the ids the tracer gave once
 *   euidaccess, eaccess                  access(), when the real and effective ids agree
 *
 * A path is resolved by the tracer's rules (policy.c): a guest path is the prefix's,
 * except the host directories the tracer leaves as they are; an absolute link target
 * is the prefix's too.  The call is then issued on the host path with the lane cookie
 * in x5 (lane.h; set for the svc only), which the filter lets through without a stop,
 * and only for calls that read:anything that writes, and any case this file does not handle (a path
 * on a deleted directory's fd, flags it does not know), goes to the real function and
 * through the tracer as before.  readlink answers are cached; the cache is dropped
 * when the generation in var/lib/deb-native/dn-tiny.gen moves (a symlinkat, unlinkat,
 * linkat or rename by any process of any session, seen by the tracer).
 *
 * libc's own internal calls (fopen's open, the loader's) do not come through these
 * symbols and stay with the tracer.
 *
 * Built against the prefix's glibc: cc -O2 -shared -fPIC -o dn-lane.so lane.c */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "lane.h"

#define LANE (((long) DN_LANE_HI << 32) | (long) DN_LANE_LO)
#define MAX_LINKS 40

static char root[PATH_MAX];             /* the prefix root, a host path; "" = not set up */
static size_t root_len;
static volatile unsigned *gen;          /* the shared link generation, or NULL: no cache */
static char cwd[PATH_MAX];              /* the guest working directory (get_cwd) */
static unsigned real_uid, real_gid;     /* read as root, as the tracer does (policy_map_uid) */

#define REAL(name) ((__typeof__(&name)) real_fn(#name, &real_##name))
static void *real_fn(const char *name, void **slot)
{
	if (!*slot)
		*slot = dlsym(RTLD_NEXT, name);
	return *slot;
}

/* A call on the lane: the cookie is in x5 for the svc only.  libc's syscall() would
 * leave it there, and the next call libc makes on its own (opendir's openat, say)
 * would pass the filter unmapped and look for the guest path on the host. */
static long lane_raw(long nr, long a, long b, long c, long d, long e)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = LANE;

	__asm__ volatile("svc #0\n\tmov x5, xzr"
			 : "+r"(x0), "+r"(x5)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
			 : "memory");
	return x0;
}

/* the same, as libc's wrappers say it: -1 and errno */
static long lane_svc(long nr, long a, long b, long c, long d, long e)
{
	long r = lane_raw(nr, a, b, c, d, e);

	if (r < 0 && r > -4096) {
		errno = (int) -r;
		return -1;
	}
	return r;
}
#define lane_call(nr, a, b, c, d, e) \
	lane_svc(nr, (long) (a), (long) (b), (long) (c), (long) (d), (long) (e))
#define lane_rawc(nr, a, b, c, d, e) \
	lane_raw(nr, (long) (a), (long) (b), (long) (c), (long) (d), (long) (e))

static void lane_atfork(void);
static void patch_libc(void);

/* ---- set up: the root from this library's own path, the shared generation ---- */
__attribute__((constructor)) static void lane_init(void)
{
	Dl_info di;
	const char *tail = "/usr/lib/deb-native/dn-lane.so";
	size_t n, t = strlen(tail);
	char p[PATH_MAX + 64];
	struct stat st;
	long fd;

	if (!dladdr((void *) lane_init, &di) || !di.dli_fname)
		return;
	n = strlen(di.dli_fname);
	if (n <= t || n - t >= sizeof root || strcmp(di.dli_fname + n - t, tail) != 0)
		return;
	if (lane_call(SYS_newfstatat, AT_FDCWD, "/proc/self", &st, 0, 0L) != 0)
		return;                                 /* the lane is not there: leave it all to the tracer */
	real_uid = st.st_uid;
	real_gid = st.st_gid;
	memcpy(p, di.dli_fname, n - t);
	strcpy(p + (n - t), "/var/lib/deb-native/dn-tiny.gen");
	fd = lane_call(SYS_openat, AT_FDCWD, p, O_RDONLY | O_CLOEXEC, 0, 0L);
	if (fd >= 0) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, (int) fd, 0);

		if (m != MAP_FAILED)
			gen = (volatile unsigned *) m;
		close((int) fd);
	}
	pthread_atfork(NULL, NULL, lane_atfork);
	memcpy(root, di.dli_fname, n - t);
	root[n - t] = '\0';
	root_len = n - t;                               /* set last: until now everything falls back */
	patch_libc();
}

/* ---- the tracer's law, in guest terms -------------------------------------- */
static int is_host_path(const char *p)
{
	static const char *const keep[] = { "/proc", "/sys", "/dev", "/data", "/system", "/storage",
					    "/sdcard", "/apex", "/vendor", "/mnt", NULL };
	int i;

	for (i = 0; keep[i]; i++) {
		size_t n = strlen(keep[i]);

		if (strncmp(p, keep[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
			return 1;
	}
	return 0;
}

/* the host path of a resolved guest path; 0, or -ENAMETOOLONG */
static int to_host(const char *g, char *host, size_t cap)
{
	size_t gl = strlen(g);

	if (is_host_path(g)) {
		if (gl >= cap)
			return -ENAMETOOLONG;
		memcpy(host, g, gl + 1);
		return 0;
	}
	if (root_len + gl >= cap)
		return -ENAMETOOLONG;
	memcpy(host, root, root_len);
	memcpy(host + root_len, g, gl + 1);
	return 0;
}

/* Threads and vfork.  The copies below are shared by every thread of the process,
 * and by a vfork or posix_spawn child until it execs.  Each is a seqlock: a reader
 * that sees a write under way, or a sequence that moved while it copied, does
 * without the copy.  What only a process can answer for itself (its working
 * directory, its ids) is also tagged with the pid it was asked for, so a vfork
 * child's answer is never its parent's; fork retags (lane_atfork). */
static int seq_begin(unsigned *seq)
{
	unsigned s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);

	return (s & 1) || !__atomic_compare_exchange_n(seq, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
	       ? -1 : (int) (s >> 1);
}

static void seq_end(unsigned *seq) { __atomic_add_fetch(seq, 1, __ATOMIC_RELEASE); }

static int seq_read(unsigned *seq, unsigned *s)
{
	*s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
	return !(*s & 1);
}

static int seq_check(unsigned *seq, unsigned s)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(seq, __ATOMIC_RELAXED) == s;
}

/* readlink of a guest path: on the host path, through the lane, from the cache.
 * "Not a link" (-EINVAL) is cached as well: it is the answer most components get. */
#define LC_SLOTS 2048
struct lc { unsigned seq, gen, hash; int r; char key[200]; char tgt[120]; };
static struct lc lcache[LC_SLOTS];

static int lc_get(struct lc *e, unsigned cur, unsigned h, const char *host, size_t len, char *buf, size_t n, long *out)
{
	unsigned s;
	int r;

	if (!seq_read(&e->seq, &s) || e->gen != cur || e->hash != h || len >= sizeof e->key ||
	    memcmp(e->key, host, len + 1) != 0)
		return 0;
	r = e->r;
	if (r > (int) sizeof e->tgt || (r > 0 && (size_t) r > n))
		return 0;
	if (r > 0)
		memcpy(buf, e->tgt, (size_t) r);
	if (!seq_check(&e->seq, s))
		return 0;
	*out = r;
	return 1;
}

static void lc_put(struct lc *e, unsigned cur, unsigned h, const char *host, size_t len, const char *buf, long r)
{
	if (len >= sizeof e->key || r >= (long) sizeof e->tgt || seq_begin(&e->seq) < 0)
		return;
	e->gen = cur;
	e->hash = h;
	e->r = (int) r;
	memcpy(e->key, host, len + 1);
	if (r > 0)
		memcpy(e->tgt, buf, (size_t) r);
	seq_end(&e->seq);
}

static int guest_readlink(const char *g, char *buf, size_t n)
{
	char host[PATH_MAX];
	unsigned h = 2166136261u, cur = gen ? *gen : 0;
	struct lc *e;
	size_t len;
	long r;

	if (to_host(g, host, sizeof host) != 0)
		return -ENAMETOOLONG;
	len = strlen(host);
	for (size_t i = 0; i < len; i++)
		h = (h ^ (unsigned char) host[i]) * 16777619u;
	e = &lcache[h % LC_SLOTS];
	if (!gen || !lc_get(e, cur, h, host, len, buf, n, &r)) {
		r = lane_call(SYS_readlinkat, AT_FDCWD, host, buf, n, 0L);
		if (r < 0)
			r = -errno;
		if (gen)
			lc_put(e, cur, h, host, len, buf, r);
	}
	/* a target the kernel spells as a host path inside the prefix is the guest's */
	if (r > 0 && (size_t) r >= root_len && strncmp(buf, root, root_len) == 0 &&
	    ((size_t) r == root_len || buf[root_len] == '/')) {
		memmove(buf, buf + root_len, (size_t) r - root_len);
		r -= (long) root_len;
		if (r == 0)
			buf[r++] = '/';
	}
	return (int) r;
}

/* ---- the working directory: asked once, dropped by chdir/fchdir ---------- */
static unsigned cwd_seq, cwd_epoch;     /* the epoch moves with every chdir */
static int cwd_pid;                     /* the process the copy is for, 0: none */
static unsigned cwd_gen;                /* the link generation it was asked under: a rename moves it */
static unsigned long cwd_dev, cwd_ino;  /* what "." was then */

/* "." and the host path of guest directory g are the same directory */
static int cwd_same(const char *g, unsigned long dev, unsigned long ino)
{
	char host[PATH_MAX];
	struct stat a, b;

	return to_host(g, host, sizeof host) == 0 &&
	       lane_call(SYS_newfstatat, AT_FDCWD, ".", &a, 0, 0L) == 0 &&
	       lane_call(SYS_newfstatat, AT_FDCWD, host, &b, 0, 0L) == 0 &&
	       a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_dev == dev && a.st_ino == ino;
}
static void *real_chdir, *real_fchdir;

static void drop_cwd(void)
{
	__atomic_add_fetch(&cwd_epoch, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&cwd_pid, 0, __ATOMIC_RELEASE);
}

static void *real_getcwd;
static char *(*real_getcwd_fn(void))(char *, size_t)
{
	return (char *(*)(char *, size_t)) real_fn("getcwd", &real_getcwd);
}

/* the guest working directory into out (PATH_MAX); 0, or -errno */
static int get_cwd(char *out)
{
	int pid = getpid();
	unsigned s, ep;

	unsigned g = gen ? *gen : 0;

	if (seq_read(&cwd_seq, &s) && __atomic_load_n(&cwd_pid, __ATOMIC_ACQUIRE) == pid) {
		unsigned cg = cwd_gen;
		unsigned long dev = cwd_dev, ino = cwd_ino;

		memcpy(out, cwd, PATH_MAX);
		if (seq_check(&cwd_seq, s) && memchr(out, 0, PATH_MAX)) {
			if (cg == g)
				return 0;
			/* a link, a rename or an unlink somewhere since: still the same directory? */
			if (cwd_same(out, dev, ino)) {
				if (seq_begin(&cwd_seq) >= 0) {
					if (cwd_dev == dev && cwd_ino == ino && strcmp(cwd, out) == 0)
						cwd_gen = g;
					seq_end(&cwd_seq);
				}
				return 0;
			}
		}
	}
	ep = __atomic_load_n(&cwd_epoch, __ATOMIC_ACQUIRE);
	{
		/* the kernel's answer is a host path: inside the prefix it is the guest's */
		long n = lane_call(SYS_getcwd, out, PATH_MAX, 0L, 0L, 0L);

		if (n < 0)
			return -errno;
		if (strncmp(out, root, root_len) == 0 && (out[root_len] == '/' || out[root_len] == '\0')) {
			if (out[root_len] == '\0')
				strcpy(out, "/");
			else
				memmove(out, out + root_len, strlen(out + root_len) + 1);
		}
	}
	if (seq_begin(&cwd_seq) >= 0) {
		if (__atomic_load_n(&cwd_epoch, __ATOMIC_ACQUIRE) == ep) {     /* no chdir meanwhile */
			struct stat d;

			strcpy(cwd, out);
			cwd_gen = g;
			if (lane_call(SYS_newfstatat, AT_FDCWD, ".", &d, 0, 0L) == 0) {
				cwd_dev = d.st_dev;
				cwd_ino = d.st_ino;
			} else {
				cwd_dev = cwd_ino = 0;
			}
			__atomic_store_n(&cwd_pid, pid, __ATOMIC_RELEASE);
		}
		seq_end(&cwd_seq);
	}
	return 0;
}

int chdir(const char *p)
{
	int r = REAL(chdir)(p);

	drop_cwd();
	return r;
}

int fchdir(int fd)
{
	int r = REAL(fchdir)(fd);

	drop_cwd();
	return r;
}

/* ---- resolving ------------------------------------------------------------ */

/* glibc's realpath algorithm (stdlib/canonicalize.c) on guest paths: each component
 * resolved, links included, then `..` against what is resolved so far.  With
 * `nofollow` the last component is left as it is (a link stays a link).  0, or -errno. */
static int resolve(const char *in, int nofollow, char *res)
{
	char rest[PATH_MAX], tgt[PATH_MAX];
	size_t rl;
	int links = 0;
	char *p;

	if (in[0] == '\0')
		return -ENOENT;
	if (strlen(in) >= sizeof rest)
		return -ENAMETOOLONG;
	if (in[0] == '/') {
		res[0] = '\0';
	} else {
		int e = get_cwd(res);

		if (e < 0)
			return e;
		if (strcmp(res, "/") == 0)
			res[0] = '\0';
	}
	rl = strlen(res);
	strcpy(rest, in);
	p = rest;
	for (;;) {
		char *comp;
		size_t cl;
		int r, last;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		comp = p;
		while (*p && *p != '/')
			p++;
		cl = (size_t) (p - comp);
		last = *p == '\0';
		if (cl == 1 && comp[0] == '.')
			continue;
		if (cl == 2 && comp[0] == '.' && comp[1] == '.') {
			while (rl > 0 && res[rl - 1] != '/')
				rl--;
			if (rl > 0)
				rl--;
			res[rl] = '\0';
			continue;
		}
		if (rl + 1 + cl >= PATH_MAX)
			return -ENAMETOOLONG;
		res[rl] = '/';
		memcpy(res + rl + 1, comp, cl);
		res[rl + 1 + cl] = '\0';
		if (last && nofollow) {
			rl += 1 + cl;
			break;
		}
		r = guest_readlink(res, tgt, sizeof tgt - 1);
		if (r == -EINVAL) {                     /* there, and not a link */
			rl += 1 + cl;
			continue;
		}
		if (r < 0)
			return r;
		if (++links > MAX_LINKS)
			return -ELOOP;
		tgt[r] = '\0';
		{
			/* the target, then what was left of the path, in place */
			size_t al = strlen(p);

			if ((size_t) r + 1 + al >= sizeof rest)
				return -ENAMETOOLONG;
			memmove(rest + r + 1, p, al + 1);
			memcpy(rest, tgt, (size_t) r);
			rest[r] = '/';
			p = rest;
		}
		if (tgt[0] == '/')
			rl = 0;                         /* an absolute target: from the root again */
		res[rl] = '\0';
	}
	if (rl == 0) {
		res[0] = '/';
		res[1] = '\0';
	}
	return 0;
}

/* The host path for a path a program passes, as the tracer would map it: links in
 * the directories resolved, the last component followed or not.  A trailing slash
 * is kept, so the kernel still says ENOTDIR for a file.  0, or -errno. */
static int host_of(const char *in, int nofollow, char *host, size_t cap)
{
	char res[PATH_MAX];
	size_t n = strlen(in);
	int r, slash = n > 0 && in[n - 1] == '/';

	r = resolve(in, nofollow && !slash, res);
	if (r == 0)
		r = to_host(res, host, cap - 1);
	if (r == 0 && slash && strcmp(res, "/") != 0)
		strcat(host, "/");
	return r;
}

/* the host path of a directory fd, as the kernel names it; its length, or 0 */
static size_t fd_host(int fd, char *out, size_t cap)
{
	char p[40] = "/proc/self/fd/", d[12];
	int k = 0;
	unsigned v = (unsigned) fd;
	long r;

	do
		d[k++] = (char) ('0' + v % 10);
	while ((v /= 10));
	for (size_t i = 14; k; )
		p[i++] = d[--k], p[i] = '\0';
	r = lane_call(SYS_readlinkat, AT_FDCWD, p, out, cap - 1, 0L);
	if (r <= 0 || out[0] != '/')
		return 0;
	out[r] = '\0';
	if (r > 10 && strcmp(out + r - 10, " (deleted)") == 0)
		return 0;
	return (size_t) r;
}

/* Where a call on (dirfd, path) goes: 1 with *at and host set (the call is then made
 * on the lane as (*at, host)), 0 when this file does not handle it (the real function,
 * through the tracer), or -errno.
 *
 * An absolute path, or one relative to the working directory, is resolved here.  On
 * a real dirfd, a one-component name goes to the kernel as it is when the last
 * component is not followed, or is not a link (a link with an absolute target would
 * be followed on the host); anything else is resolved from the directory's guest
 * path.  A directory outside the prefix is a host directory: its paths stay. */
static int lane_at(int dirfd, const char *path, int nofollow, char *host, size_t cap, int *at)
{
	char dir[PATH_MAX], guest[PATH_MAX];
	size_t dl, pl;

	if (!root_len || !path || path[0] == '\0')
		return 0;
	*at = AT_FDCWD;
	if (path[0] == '/' || dirfd == AT_FDCWD) {
		int e = host_of(path, nofollow, host, cap);

		return e < 0 ? e : 1;
	}
	if (dirfd < 0)
		return 0;
	pl = strlen(path);
	if (pl >= cap)
		return -ENAMETOOLONG;
	if (!strchr(path, '/') && strcmp(path, ".") != 0 && strcmp(path, "..") != 0) {
		char t[2];

		if (nofollow || lane_call(SYS_readlinkat, dirfd, path, t, sizeof t, 0L) < 0) {
			memcpy(host, path, pl + 1);     /* not followed, not a link, or not there */
			*at = dirfd;
			return 1;
		}
	}
	dl = fd_host(dirfd, dir, sizeof dir);
	if (dl == 0)
		return 0;
	if (strncmp(dir, root, root_len) != 0 || (dir[root_len] != '/' && dir[root_len] != '\0')) {
		memcpy(host, path, pl + 1);             /* a host directory */
		*at = dirfd;
		return 1;
	}
	if ((dl - root_len) + 1 + pl >= sizeof guest)
		return -ENAMETOOLONG;
	memcpy(guest, dir + root_len, dl - root_len);
	guest[dl - root_len] = '/';
	memcpy(guest + (dl - root_len) + 1, path, pl + 1);
	{
		int e = host_of(guest, nofollow, host, cap);

		return e < 0 ? e : 1;
	}
}

/* ---- realpath ------------------------------------------------------------- */
static void *real_realpath;

static char *do_realpath(const char *in, char *out)
{
	char res[PATH_MAX];
	int r;

	if (!root_len)
		return REAL(realpath)(in, out);
	if (in == NULL) {
		errno = EINVAL;
		return NULL;
	}
	r = resolve(in, 0, res);
	if (r < 0) {
		errno = -r;
		return NULL;
	}
	if (out) {
		strcpy(out, res);
		return out;
	}
	return strdup(res);
}

char *realpath(const char *in, char *out) { return do_realpath(in, out); }
char *canonicalize_file_name(const char *in) { return do_realpath(in, NULL); }

/* _FORTIFY_SOURCE builds call this one */
char *__realpath_chk(const char *in, char *out, size_t outlen)
{
	if (out && outlen < PATH_MAX)
		abort();                                /* what glibc does: a buffer overflow */
	return do_realpath(in, out);
}

/* ---- open ------------------------------------------------------------------ */
static void *real_openat;

static int lane_open(int dirfd, const char *path, int flags, mode_t mode)
{
	char host[PATH_MAX];
	int at, e;

	if (flags & DN_LANE_OPEN_DENY)
		return REAL(openat)(dirfd, path, flags, mode);
	e = lane_at(dirfd, path, flags & O_NOFOLLOW, host, sizeof host, &at);
	if (e == 0)
		return REAL(openat)(dirfd, path, flags, mode);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	return (int) lane_call(SYS_openat, at, host, flags, mode, 0L);
}

#define MODE_ARG(flags, mode)                                           \
	do {                                                            \
		if ((flags) & (O_CREAT | __O_TMPFILE)) {                \
			va_list ap;                                     \
			va_start(ap, flags);                            \
			mode = (mode_t) va_arg(ap, int);                \
			va_end(ap);                                     \
		}                                                       \
	} while (0)

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	MODE_ARG(flags, mode);
	return lane_open(AT_FDCWD, path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;

	MODE_ARG(flags, mode);
	return lane_open(AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;

	MODE_ARG(flags, mode);
	return lane_open(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;

	MODE_ARG(flags, mode);
	return lane_open(dirfd, path, flags, mode);
}

/* _FORTIFY_SOURCE: open without a mode, checked */
int __open_2(const char *path, int flags) { return lane_open(AT_FDCWD, path, flags, 0); }
int __open64_2(const char *path, int flags) { return lane_open(AT_FDCWD, path, flags, 0); }
int __openat_2(int dirfd, const char *path, int flags) { return lane_open(dirfd, path, flags, 0); }
int __openat64_2(int dirfd, const char *path, int flags) { return lane_open(dirfd, path, flags, 0); }

/* ---- stat ------------------------------------------------------------------ */
static void *real_fstatat, *real_statx;

/* a link's size as the program will read its target: the kernel's spelling of a
 * path inside the prefix is shortened to the guest's */
static long link_size(int at, const char *host, long size)
{
	char tgt[PATH_MAX];
	long r = lane_call(SYS_readlinkat, at, host, tgt, sizeof tgt - 1, 0L);

	if (r <= 0)
		return size;
	if ((size_t) r >= root_len && strncmp(tgt, root, root_len) == 0 &&
	    ((size_t) r == root_len || tgt[root_len] == '/'))
		return (size_t) r == root_len ? 1 : r - (long) root_len;
	return r;
}

/* ---- hard links: Android refuses them; the tracer copies the file and keeps their
 * link count for the stats it answers.  The stats this file answers see a process's
 * own links: linkat records the two copies, unlinkat drops a name, and a stat of one
 * says how many names its group has (shadow links a temporary name to lock its files
 * and checks for a count of 2).  No table, no cost: most processes never link. */
#define CL_MAX 16
static struct { unsigned long dev, ino; int group; } cl[CL_MAX];
static int ncl, cl_lock, cl_next = 1;

static int cl_take(int wait)
{
	while (__atomic_exchange_n(&cl_lock, 1, __ATOMIC_ACQUIRE))
		if (!wait)
			return 0;                       /* a signal handler: do without */
	return 1;
}

static void cl_give(void) { __atomic_store_n(&cl_lock, 0, __ATOMIC_RELEASE); }

static int cl_find(unsigned long dev, unsigned long ino)
{
	for (int i = 0; i < ncl; i++)
		if (cl[i].dev == dev && cl[i].ino == ino)
			return i;
	return -1;
}

static void cl_put(unsigned long dev, unsigned long ino, int group)
{
	if (ncl == CL_MAX) {
		memmove(&cl[0], &cl[1], sizeof cl[0] * (CL_MAX - 1));
		ncl--;
	}
	cl[ncl].dev = dev;
	cl[ncl].ino = ino;
	cl[ncl].group = group;
	__atomic_store_n(&ncl, ncl + 1, __ATOMIC_RELEASE);
}

/* the link count to report for (dev, ino), or 0 */
static unsigned cl_count(unsigned long dev, unsigned long ino)
{
	unsigned n = 0;
	int i;

	if (!__atomic_load_n(&ncl, __ATOMIC_ACQUIRE) || !cl_take(0))
		return 0;
	if ((i = cl_find(dev, ino)) >= 0)
		for (int j = 0; j < ncl; j++)
			n += cl[j].group == cl[i].group;
	cl_give();
	return n;
}

static void cl_fix(unsigned long dev, unsigned long ino, unsigned *nlink)
{
	unsigned n = cl_count(dev, ino);

	if (n > *nlink)
		*nlink = n;
}

/* what the program is told: the owner read as root */
static void map_owner(unsigned *uid, unsigned *gid)
{
	if (*uid == real_uid)
		*uid = 0;
	if (*gid == real_gid)
		*gid = 0;
}

static int lane_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	char host[PATH_MAX];
	int nofollow = flags & AT_SYMLINK_NOFOLLOW, at, e;

	if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0' && dirfd >= 0) {
		if (!root_len)
			return REAL(fstatat)(dirfd, path, st, flags);
		if (lane_call(SYS_fstat, dirfd, st, 0L, 0L, 0L) != 0)
			return -1;
		map_owner(&st->st_uid, &st->st_gid);
		cl_fix(st->st_dev, st->st_ino, &st->st_nlink);
		return 0;
	}
	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT))
		return REAL(fstatat)(dirfd, path, st, flags);
	e = lane_at(dirfd, path, nofollow, host, sizeof host, &at);
	if (e == 0)
		return REAL(fstatat)(dirfd, path, st, flags);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	if (lane_call(SYS_newfstatat, at, host, st, flags, 0L) != 0)
		return -1;
	map_owner(&st->st_uid, &st->st_gid);
	cl_fix(st->st_dev, st->st_ino, &st->st_nlink);
	if (nofollow && S_ISLNK(st->st_mode))
		st->st_size = link_size(at, host, st->st_size);
	return 0;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) { return lane_fstatat(dirfd, path, st, flags); }
int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) { return lane_fstatat(dirfd, path, (struct stat *) st, flags); }
int stat(const char *path, struct stat *st) { return lane_fstatat(AT_FDCWD, path, st, 0); }
int stat64(const char *path, struct stat64 *st) { return lane_fstatat(AT_FDCWD, path, (struct stat *) st, 0); }
int lstat(const char *path, struct stat *st) { return lane_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW); }
int lstat64(const char *path, struct stat64 *st) { return lane_fstatat(AT_FDCWD, path, (struct stat *) st, AT_SYMLINK_NOFOLLOW); }
int fstat(int fd, struct stat *st) { return lane_fstatat(fd, "", st, AT_EMPTY_PATH); }
int fstat64(int fd, struct stat64 *st) { return lane_fstatat(fd, "", (struct stat *) st, AT_EMPTY_PATH); }

/* statx: the same, with the sync type kept */
int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *stx)
{
	char host[PATH_MAX];
	int nofollow = flags & AT_SYMLINK_NOFOLLOW, at, e;

	if (!root_len || (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)))
		return REAL(statx)(dirfd, path, flags, mask, stx);
	if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
		at = dirfd;
		host[0] = '\0';
	} else {
		e = lane_at(dirfd, path, nofollow, host, sizeof host, &at);
		if (e == 0)
			return REAL(statx)(dirfd, path, flags, mask, stx);
		if (e < 0) {
			errno = -e;
			return -1;
		}
		flags &= ~AT_EMPTY_PATH;
	}
	if (lane_call(SYS_statx, at, host, flags, mask, stx) != 0)
		return -1;
	map_owner(&stx->stx_uid, &stx->stx_gid);
	cl_fix(makedev(stx->stx_dev_major, stx->stx_dev_minor), stx->stx_ino, &stx->stx_nlink);
	if (nofollow && host[0] && S_ISLNK(stx->stx_mode))
		stx->stx_size = (unsigned long) link_size(at, host, (long) stx->stx_size);
	return 0;
}

/* ---- access ---------------------------------------------------------------- */
static void *real_faccessat;

static int lane_faccessat(int dirfd, const char *path, int mode, int flags)
{
	char host[PATH_MAX];
	int at, e;

	if (flags != 0)
		return REAL(faccessat)(dirfd, path, mode, flags);
	e = lane_at(dirfd, path, 0, host, sizeof host, &at);
	if (e == 0)
		return REAL(faccessat)(dirfd, path, mode, flags);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	return (int) lane_call(SYS_faccessat, at, host, mode, 0L, 0L);
}

int faccessat(int dirfd, const char *path, int mode, int flags) { return lane_faccessat(dirfd, path, mode, flags); }
int access(const char *path, int mode) { return lane_faccessat(AT_FDCWD, path, mode, 0); }

/* ---- readlink -------------------------------------------------------------- */
/* /proc is left to the tracer: /proc/<pid>/exe names the loader to the kernel, and
 * the tracer knows the program. */
static void *real_readlinkat;

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t n)
{
	char host[PATH_MAX], tgt[PATH_MAX];
	int at, e;
	long r;

	if (strncmp(path, "/proc/", 6) == 0 || n == 0)
		return REAL(readlinkat)(dirfd, path, buf, n);
	e = lane_at(dirfd, path, 1, host, sizeof host, &at);
	if (e == 0 || (at != AT_FDCWD && strncmp(host, "/proc/", 6) == 0))
		return REAL(readlinkat)(dirfd, path, buf, n);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	r = lane_call(SYS_readlinkat, at, host, tgt, sizeof tgt, 0L);
	if (r < 0)
		return -1;
	if ((size_t) r >= root_len && strncmp(tgt, root, root_len) == 0 &&
	    ((size_t) r == root_len || tgt[root_len] == '/')) {
		memmove(tgt, tgt + root_len, (size_t) r - root_len);
		r -= (long) root_len;
		if (r == 0)
			tgt[r++] = '/';
	}
	if ((size_t) r > n)
		r = (long) n;
	memcpy(buf, tgt, (size_t) r);
	return r;
}

ssize_t readlink(const char *path, char *buf, size_t n) { return readlinkat(AT_FDCWD, path, buf, n); }

/* ---- xattr by path (ls -l asks two for every name) -------------------------- */
static void *real_getxattr, *real_lgetxattr, *real_listxattr, *real_llistxattr;

static ssize_t lane_xattr(long nr, const char *path, int nofollow, long a, long b, long c, int *handled)
{
	char host[PATH_MAX];
	int at, e = lane_at(AT_FDCWD, path, nofollow, host, sizeof host, &at);

	*handled = e != 0;
	if (e < 0) {
		errno = -e;
		return -1;
	}
	return e ? lane_call(nr, host, a, b, c, 0L) : 0;
}

ssize_t getxattr(const char *path, const char *name, void *v, size_t size)
{
	int h;
	ssize_t r = lane_xattr(SYS_getxattr, path, 0, (long) name, (long) v, (long) size, &h);

	return h ? r : REAL(getxattr)(path, name, v, size);
}

ssize_t lgetxattr(const char *path, const char *name, void *v, size_t size)
{
	int h;
	ssize_t r = lane_xattr(SYS_lgetxattr, path, 1, (long) name, (long) v, (long) size, &h);

	return h ? r : REAL(lgetxattr)(path, name, v, size);
}

ssize_t listxattr(const char *path, char *list, size_t size)
{
	int h;
	ssize_t r = lane_xattr(SYS_listxattr, path, 0, (long) list, (long) size, 0L, &h);

	return h ? r : REAL(listxattr)(path, list, size);
}

ssize_t llistxattr(const char *path, char *list, size_t size)
{
	int h;
	ssize_t r = lane_xattr(SYS_llistxattr, path, 1, (long) list, (long) size, 0L, &h);

	return h ? r : REAL(llistxattr)(path, list, size);
}

/* ---- the working directory, as getcwd says it -------------------------------- */

char *getcwd(char *buf, size_t size)
{
	char g[PATH_MAX];
	size_t n;
	int e;

	if (!root_len)
		return real_getcwd_fn()(buf, size);
	e = get_cwd(g);
	if (e < 0) {
		errno = -e;
		return NULL;
	}
	n = strlen(g) + 1;
	if (!buf) {
		if (size == 0)
			size = n;
		if (size < n) {
			errno = ERANGE;
			return NULL;
		}
		buf = malloc(size);
		if (!buf)
			return NULL;
	} else if (size < n) {
		errno = size == 0 ? EINVAL : ERANGE;
		return NULL;
	}
	memcpy(buf, g, n);
	return buf;
}

/* ---- identity: asked of the tracer once, answered from a copy -------------- */
/* The tracer keeps each process's ids (fake root: getresuid and getresgid are its
 * rows); the shim asks once, with x5 clear so the calls stop, and answers getuid and
 * the rest from what it was told.  A set*id goes to the tracer and drops the copy. */
static unsigned ids[6];                 /* ruid euid suid rgid egid sgid */
static unsigned ids_seq, ids_epoch;     /* the epoch moves with every set*id */
static int ids_pid;                     /* the process the copy is for, 0: none */

static long plain_svc(long nr, long a, long b, long c)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x5 __asm__("x5") = 0;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x5) : "memory");
	return x0;
}

/* this process's ids into v; 1, or 0 when the tracer could not be asked */
static int get_ids(unsigned v[6])
{
	int pid = getpid();
	unsigned s, ep;

	if (seq_read(&ids_seq, &s) && __atomic_load_n(&ids_pid, __ATOMIC_ACQUIRE) == pid) {
		memcpy(v, ids, sizeof ids);
		if (seq_check(&ids_seq, s))
			return 1;
	}
	ep = __atomic_load_n(&ids_epoch, __ATOMIC_ACQUIRE);
	if (plain_svc(SYS_getresuid, (long) &v[0], (long) &v[1], (long) &v[2]) != 0 ||
	    plain_svc(SYS_getresgid, (long) &v[3], (long) &v[4], (long) &v[5]) != 0)
		return 0;
	if (seq_begin(&ids_seq) >= 0) {
		if (__atomic_load_n(&ids_epoch, __ATOMIC_ACQUIRE) == ep) {     /* no set*id meanwhile */
			memcpy(ids, v, sizeof ids);
			__atomic_store_n(&ids_pid, pid, __ATOMIC_RELEASE);
		}
		seq_end(&ids_seq);
	}
	return 1;
}

static void drop_ids(void)
{
	__atomic_add_fetch(&ids_epoch, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&ids_pid, 0, __ATOMIC_RELEASE);
}

/* a forked child has its parent's working directory and ids: keep the copies */
static void lane_atfork(void)
{
	int pid = getpid();

	if (__atomic_load_n(&cwd_pid, __ATOMIC_ACQUIRE) != 0)
		__atomic_store_n(&cwd_pid, pid, __ATOMIC_RELEASE);
	if (__atomic_load_n(&ids_pid, __ATOMIC_ACQUIRE) != 0)
		__atomic_store_n(&ids_pid, pid, __ATOMIC_RELEASE);
}

static void *real_getuid, *real_geteuid, *real_getgid, *real_getegid, *real_getresuid, *real_getresgid;

uid_t getuid(void) { unsigned v[6]; return root_len && get_ids(v) ? v[0] : REAL(getuid)(); }
uid_t geteuid(void) { unsigned v[6]; return root_len && get_ids(v) ? v[1] : REAL(geteuid)(); }
gid_t getgid(void) { unsigned v[6]; return root_len && get_ids(v) ? v[3] : REAL(getgid)(); }
gid_t getegid(void) { unsigned v[6]; return root_len && get_ids(v) ? v[4] : REAL(getegid)(); }

int getresuid(uid_t *r, uid_t *e, uid_t *s)
{
	unsigned v[6];

	if (!root_len || !get_ids(v))
		return REAL(getresuid)(r, e, s);
	*r = v[0];
	*e = v[1];
	*s = v[2];
	return 0;
}

int getresgid(gid_t *r, gid_t *e, gid_t *s)
{
	unsigned v[6];

	if (!root_len || !get_ids(v))
		return REAL(getresgid)(r, e, s);
	*r = v[3];
	*e = v[4];
	*s = v[5];
	return 0;
}

#define SET_ID(name, params, args)                              \
	static void *real_##name;                               \
	int name params                                         \
	{                                                       \
		int ret_ = REAL(name) args;                     \
		drop_ids();                                     \
		return ret_;                                    \
	}
SET_ID(setuid, (uid_t u), (u))
SET_ID(seteuid, (uid_t u), (u))
SET_ID(setreuid, (uid_t r, uid_t e), (r, e))
SET_ID(setresuid, (uid_t r, uid_t e, uid_t s), (r, e, s))
SET_ID(setgid, (gid_t g), (g))
SET_ID(setegid, (gid_t g), (g))
SET_ID(setregid, (gid_t r, gid_t e), (r, e))
SET_ID(setresgid, (gid_t r, gid_t e, gid_t s), (r, e, s))

/* euidaccess and eaccess ask libc's own get*id (four stops a call, from make and
 * bash looking for commands).  With the real and effective ids equal, glibc's answer
 * is access(); the shim says the same from its copy. */
static void *real_euidaccess;

int euidaccess(const char *path, int mode)
{
	unsigned v[6];

	if (root_len && get_ids(v) && v[0] == v[1] && v[3] == v[4])
		return lane_faccessat(AT_FDCWD, path, mode, 0);
	return REAL(euidaccess)(path, mode);
}

int eaccess(const char *path, int mode) { return euidaccess(path, mode); }

/* ---- libc's own calls: its svc sites, patched at load --------------------- */
/* libc's internal calls (opendir and the locale's opens through __open_nocancel,
 * fstatat, getcwd, the ids) do not come through the symbols above.  At set-up each
 * `mov x8, #nr; svc #0` in libc's text whose nr this file answers becomes a branch to
 * a stub near libc; the stub calls lane_hook with every register saved, and either
 * returns its answer in x0 or makes the original call.  Only x0 changes, as with the
 * svc.  The hook reads x8 when the call is made, so a site picked wrongly costs a
 * plain svc, no more.  libc's cancellable path (the number in a register) is left
 * alone: pthread_cancel looks for the program counter there.
 * DN_LANE_PATCH=0 leaves libc as it is. */
#include <link.h>
#include <sys/syscall.h>

#define HOOK_FRAME 272                  /* x0-x30 (x29, x30 at 232), nzcv at 248, lr at 256 */

/* a call libc makes by itself: 1 with the answer in r[0] (a result or -errno),
 * 0 to make the call as it is */
static int lane_hook(unsigned long *r)
{
	char host[PATH_MAX];
	int saved = errno, at, e, done = 1;
	long nr = (long) r[8], res = 0;
	const char *path = (const char *) r[1];

	switch (nr) {
	case SYS_openat:
		if (r[2] & DN_LANE_OPEN_DENY) {
			done = 0;
			break;
		}
		e = lane_at((int) r[0], path, (int) r[2] & O_NOFOLLOW, host, sizeof host, &at);
		if (e <= 0) {
			done = e < 0;
			res = e;
			break;
		}
		res = lane_rawc(SYS_openat, at, host, r[2], r[3], 0L);
		break;
	case SYS_faccessat:
		e = lane_at((int) r[0], path, 0, host, sizeof host, &at);
		if (e <= 0) {
			done = e < 0;
			res = e;
			break;
		}
		res = lane_rawc(SYS_faccessat, at, host, r[2], 0L, 0L);
		break;
	case SYS_newfstatat:
	case SYS_statx: {
		int fl = (int) (nr == SYS_statx ? r[2] : r[3]), nofollow = fl & AT_SYMLINK_NOFOLLOW;
		int ok = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | (nr == SYS_statx ? AT_STATX_SYNC_TYPE : 0);

		if (fl & ~ok) {
			done = 0;
			break;
		}
		if ((fl & AT_EMPTY_PATH) && path && path[0] == '\0') {
			at = (int) r[0];
			host[0] = '\0';
		} else {
			e = lane_at((int) r[0], path, nofollow, host, sizeof host, &at);
			if (e <= 0) {
				done = e < 0;
				res = e;
				break;
			}
			fl &= ~AT_EMPTY_PATH;
		}
		if (nr == SYS_statx) {
			struct statx *x = (struct statx *) r[4];

			res = lane_rawc(SYS_statx, at, host, fl, r[3], x);
			if (res == 0) {
				map_owner(&x->stx_uid, &x->stx_gid);
				cl_fix(makedev(x->stx_dev_major, x->stx_dev_minor), x->stx_ino, &x->stx_nlink);
				if (nofollow && host[0] && S_ISLNK(x->stx_mode))
					x->stx_size = (unsigned long) link_size(at, host, (long) x->stx_size);
			}
		} else {
			struct stat *st = (struct stat *) r[2];

			res = lane_rawc(SYS_newfstatat, at, host, st, fl, 0L);
			if (res == 0) {
				map_owner(&st->st_uid, &st->st_gid);
				cl_fix(st->st_dev, st->st_ino, &st->st_nlink);
				if (nofollow && host[0] && S_ISLNK(st->st_mode))
					st->st_size = link_size(at, host, st->st_size);
			}
		}
		break;
	}
	case SYS_readlinkat: {
		char tgt[PATH_MAX];
		size_t n = (size_t) r[3];

		if (!path || strncmp(path, "/proc/", 6) == 0 || n == 0) {
			done = 0;
			break;
		}
		e = lane_at((int) r[0], path, 1, host, sizeof host, &at);
		if (e <= 0 || (at != AT_FDCWD && strncmp(host, "/proc/", 6) == 0)) {
			done = e < 0;
			res = e;
			break;
		}
		res = lane_rawc(SYS_readlinkat, at, host, tgt, sizeof tgt, 0L);
		if (res > 0) {
			if ((size_t) res >= root_len && strncmp(tgt, root, root_len) == 0 &&
			    ((size_t) res == root_len || tgt[root_len] == '/')) {
				memmove(tgt, tgt + root_len, (size_t) res - root_len);
				res -= (long) root_len;
				if (res == 0)
					tgt[res++] = '/';
			}
			if ((size_t) res > n)
				res = (long) n;
			memcpy((char *) r[2], tgt, (size_t) res);
		}
		break;
	}
	case SYS_getcwd: {
		char g[PATH_MAX];
		size_t n;

		e = get_cwd(g);
		if (e < 0) {
			res = e;
			break;
		}
		n = strlen(g) + 1;
		if (n > (size_t) r[1]) {
			res = -ERANGE;
			break;
		}
		memcpy((char *) r[0], g, n);
		res = (long) n;
		break;
	}
	case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid: {
		unsigned v[6];

		if (!get_ids(v)) {
			done = 0;
			break;
		}
		res = v[nr == SYS_getuid ? 0 : nr == SYS_geteuid ? 1 : nr == SYS_getgid ? 3 : 4];
		break;
	}
	default:
		done = 0;
	}
	errno = saved;
	if (done)
		r[0] = (unsigned long) res;
	return done;
}

/* The hook runs on a stack of its own, one per thread: libc's calls come from
 * anywhere, posix_spawn's child among them, which runs on a small stack glibc
 * allocates (64 KB and the arguments), and the hook's path work needs tens of
 * KB.  The stack is mapped at a thread's first hook and unmapped when the
 * thread ends; a vfork or posix_spawn child, which runs in its parent's thread,
 * uses its parent's while the parent waits. */
#define HOOK_STACK (128 * 1024)
static __thread char *hook_stack __attribute__((tls_model("initial-exec")));
static pthread_key_t hook_key;

/* lane_on_stack(r, fn, top): fn(r) with sp at top, the caller's sp kept there */
int lane_on_stack(unsigned long *r, int (*fn)(unsigned long *), void *top);
__asm__(".text\n"
	".p2align 2\n"
	".type lane_on_stack, %function\n"
	"lane_on_stack:\n"
	"	mov x9, sp\n"
	"	mov sp, x2\n"
	"	stp x9, x30, [sp, #-16]!\n"
	"	blr x1\n"
	"	ldp x9, x30, [sp], #16\n"
	"	mov sp, x9\n"
	"	ret\n"
	".size lane_on_stack, .-lane_on_stack\n");

static void hook_stack_free(void *s) { munmap(s, HOOK_STACK); }

static int lane_hook_entry(unsigned long *r)
{
	char *s = hook_stack, here;

	if (!s) {
		void *m = mmap(NULL, HOOK_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

		if (m == MAP_FAILED)
			return 0;                       /* make the call as it is */
		hook_stack = s = m;
		pthread_setspecific(hook_key, m);
	}
	if (&here >= s && &here < s + HOOK_STACK)
		return lane_hook(r);                    /* already on it: a signal during a hook */
	return lane_on_stack(r, lane_hook, s + HOOK_STACK);
}

/* ---- the code the stubs run, encoded here (no assembler at run time) ---- */
static unsigned *emit_p;
static void emit(unsigned insn) { *emit_p++ = insn; }
static unsigned stp(int a, int b, int off) { return 0xa9000000u | ((unsigned) (off / 8) & 0x7f) << 15 | (unsigned) b << 10 | 31u << 5 | (unsigned) a; }
static unsigned ldp(int a, int b, int off) { return 0xa9400000u | ((unsigned) (off / 8) & 0x7f) << 15 | (unsigned) b << 10 | 31u << 5 | (unsigned) a; }
static unsigned strx(int t, int off) { return 0xf9000000u | (unsigned) (off / 8) << 10 | 31u << 5 | (unsigned) t; }
static unsigned ldrx(int t, int off) { return 0xf9400000u | (unsigned) (off / 8) << 10 | 31u << 5 | (unsigned) t; }
static unsigned branch(unsigned op, const void *from, const void *to)
{
	long d = ((const char *) to - (const char *) from) / 4;

	return op | ((unsigned) d & 0x3ffffffu);
}
static int in_reach(const void *a, const void *b)
{
	long d = (const char *) a - (const char *) b;

	return d > -(120L << 20) && d < (120L << 20);
}

/* the shared part: save, call lane_hook, restore; make the call if it said 0 */
static void emit_common(void)
{
	unsigned *cbz, *lit;
	int i;

	for (i = 0; i < 28; i += 2)
		emit(stp(i, i + 1, i * 8));
	emit(strx(28, 224));
	emit(0xd53b4209u);                              /* mrs x9, nzcv */
	emit(strx(9, 248));
	emit(strx(30, 256));
	emit(0x910003e0u);                              /* mov x0, sp */
	lit = emit_p;
	emit(0);                                        /* ldr x16, =lane_hook_entry (set below) */
	emit(0xd63f0200u);                              /* blr x16 */
	emit(ldrx(9, 248));
	emit(0xd51b4209u);                              /* msr nzcv, x9 */
	emit(ldrx(30, 256));
	cbz = emit_p;
	emit(0);                                        /* cbz x0, passthrough (set below) */
	emit(ldrx(0, 0));                               /* answered: x0 from the frame */
	emit(ldrx(1, 8));
	for (i = 2; i < 28; i += 2)
		emit(ldp(i, i + 1, i * 8));
	emit(ldrx(28, 224));
	emit(0xd65f03c0u);                              /* ret */
	*cbz = 0xb4000000u | ((unsigned) (emit_p - cbz) & 0x7ffff) << 5;
	for (i = 0; i < 28; i += 2)
		emit(ldp(i, i + 1, i * 8));
	emit(ldrx(28, 224));
	emit(0xd4000001u);                              /* svc #0 */
	emit(0xd65f03c0u);                              /* ret */
	if ((unsigned long) emit_p & 4)
		emit(0xd503201fu);                      /* nop: the literal is 8-aligned */
	*lit = 0x58000000u | ((unsigned) (emit_p - lit) & 0x7ffff) << 5 | 16u;
	*(void **) emit_p = (void *) lane_hook_entry;
	emit_p += 2;
}

static int hooked_nr(unsigned nr)
{
	switch (nr) {
	case SYS_openat: case SYS_faccessat: case SYS_newfstatat: case SYS_statx: case SYS_readlinkat:
	case SYS_getcwd: case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
		return 1;
	}
	return 0;
}

struct text { unsigned *lo, *hi; };

static int find_libc(struct dl_phdr_info *info, size_t size, void *data)
{
	struct text *t = data;
	size_t n = info->dlpi_name ? strlen(info->dlpi_name) : 0;
	int i;

	(void) size;
	if (n < 10 || strcmp(info->dlpi_name + n - 10, "/libc.so.6") != 0)
		return 0;
	for (i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

		if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
			t->lo = (unsigned *) (info->dlpi_addr + ph->p_vaddr);
			t->hi = (unsigned *) (info->dlpi_addr + ph->p_vaddr + ph->p_filesz);
			return 1;
		}
	}
	return 0;
}

static void patch_libc(void)
{
	struct text t = { 0, 0 };
	const char *knob = getenv("DN_LANE_PATCH");
	unsigned *p, *common, *page = MAP_FAILED;
	size_t sites = 0, len = 4096 * 4;
	unsigned long lo, hi;
	int k;

	if ((knob && strcmp(knob, "0") == 0) || !dl_iterate_phdr(find_libc, &t) || !t.lo)
		return;
	if (pthread_key_create(&hook_key, hook_stack_free) != 0)
		return;
	for (p = t.lo + 1; p < t.hi; p++)
		if (*p == 0xd4000001u && (p[-1] & 0xffe0001fu) == 0xd2800008u && hooked_nr((p[-1] >> 5) & 0xffff))
			sites++;
	if (sites == 0 || 128 + sites * 6 > len / 4)
		return;
	for (k = 1; k <= 8 && page == MAP_FAILED; k++) {      /* a page the stubs can reach: near the text */
		char *hint = (char *) (((unsigned long) t.hi + ((unsigned long) k << 21)) & ~0xfffffUL);

		page = mmap(hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page != MAP_FAILED && (!in_reach(page, t.lo) || !in_reach(page + len / 4, t.hi))) {
			munmap(page, len);
			page = MAP_FAILED;
		}
	}
	if (page == MAP_FAILED)
		return;
	emit_p = common = page;
	emit_common();
	lo = (unsigned long) t.lo & ~4095UL;
	hi = ((unsigned long) t.hi + 4095) & ~4095UL;
	{
		unsigned *stubs = emit_p;

		for (p = t.lo + 1; p < t.hi; p++) {
			if (!(*p == 0xd4000001u && (p[-1] & 0xffe0001fu) == 0xd2800008u && hooked_nr((p[-1] >> 5) & 0xffff)))
				continue;
			emit(0xd10443ffu);                              /* sub sp, sp, #272 */
			emit(stp(29, 30, 232));
			emit(branch(0x94000000u, emit_p, common));      /* bl common */
			emit(ldp(29, 30, 232));
			emit(0x910443ffu);                              /* add sp, sp, #272 */
			emit(branch(0x14000000u, emit_p, p + 1));       /* b site + 4 */
		}
		__builtin___clear_cache((char *) page, (char *) emit_p);
		if (mprotect(page, len, PROT_READ | PROT_EXEC) != 0)
			return;
		/* libc's own mprotect may sit in the text being made writable: call the kernel */
		if (plain_svc(SYS_mprotect, (long) lo, (long) (hi - lo), PROT_READ | PROT_WRITE | PROT_EXEC) != 0 &&
		    plain_svc(SYS_mprotect, (long) lo, (long) (hi - lo), PROT_READ | PROT_WRITE) != 0)
			return;
		for (p = t.lo + 1; p < t.hi; p++) {
			if (!(*p == 0xd4000001u && (p[-1] & 0xffe0001fu) == 0xd2800008u && hooked_nr((p[-1] >> 5) & 0xffff)))
				continue;
			*p = branch(0x14000000u, p, stubs);
			stubs += 6;
		}
		__builtin___clear_cache((char *) lo, (char *) hi);
		plain_svc(SYS_mprotect, (long) lo, (long) (hi - lo), PROT_READ | PROT_EXEC);
	}
}

/* ---- link and unlink, for the link counts (see cl_count) ------------------ */
static void *real_linkat, *real_unlinkat;

int linkat(int od, const char *op, int nd, const char *np, int flags)
{
	struct stat a, b;
	int r = REAL(linkat)(od, op, nd, np, flags), e = errno;

	if (r == 0 && root_len &&
	    lane_fstatat(od, op, &a, (flags & AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW) == 0 &&
	    lane_fstatat(nd, np, &b, AT_SYMLINK_NOFOLLOW) == 0 && (a.st_dev != b.st_dev || a.st_ino != b.st_ino)) {
		int i, g;

		cl_take(1);
		if ((i = cl_find(a.st_dev, a.st_ino)) >= 0) {
			g = cl[i].group;
		} else {
			g = cl_next++;
			cl_put(a.st_dev, a.st_ino, g);
		}
		cl_put(b.st_dev, b.st_ino, g);
		cl_give();
	}
	errno = e;
	return r;
}

int link(const char *op, const char *np) { return linkat(AT_FDCWD, op, AT_FDCWD, np, 0); }

int unlinkat(int dirfd, const char *path, int flags)
{
	struct stat st;
	int known = 0, r, e;

	if (__atomic_load_n(&ncl, __ATOMIC_ACQUIRE) && !(flags & AT_REMOVEDIR) &&
	    lane_fstatat(dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == 0)
		known = 1;
	r = REAL(unlinkat)(dirfd, path, flags);
	e = errno;
	if (r == 0 && known) {
		int i;

		cl_take(1);
		if ((i = cl_find(st.st_dev, st.st_ino)) >= 0) {
			memmove(&cl[i], &cl[i + 1], sizeof cl[0] * (size_t) (ncl - i - 1));
			ncl--;
		}
		cl_give();
	}
	errno = e;
	return r;
}

int unlink(const char *path) { return unlinkat(AT_FDCWD, path, 0); }
