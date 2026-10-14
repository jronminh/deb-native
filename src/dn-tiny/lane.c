/* lane.c -- dn-lane.so, the shim of dn-tiny: the exec gate preloads it into every
 * glibc program it starts (ld.so --preload).  It does the tracer's path work inside
 * the program, for the calls that only read, so that they cost no stop:
 *
 *   realpath, canonicalize_file_name     resolved here, one cached readlink a component
 *   open, openat (read-only flags)       opened here, so the fd is the program's own
 *   stat, lstat, fstatat, fstat          stat here, the owner read as root, a link's size
 *   access, faccessat (no flags)         checked here
 *
 * A path is resolved by the tracer's rules (policy.c): a guest path is the prefix's,
 * except the host directories the tracer leaves as they are; an absolute link target
 * is the prefix's too.  The call is then issued on the host path with the lane cookie
 * in x5 (lane.h), which the filter lets through without a stop, and only for calls
 * that read: anything that writes, and any case this file does not handle (a path
 * relative to a directory fd, flags it does not know), goes to the real function and
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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "lane.h"

#define LANE (((long) DN_LANE_HI << 32) | (long) DN_LANE_LO)
#define MAX_LINKS 40

static char root[PATH_MAX];             /* the prefix root, a host path; "" = not set up */
static size_t root_len;
static volatile unsigned *gen;          /* the shared link generation, or NULL: no cache */
static char cwd[PATH_MAX];              /* the guest working directory, "" = unknown */
static unsigned real_uid, real_gid;     /* read as root, as the tracer does (policy_map_uid) */

#define REAL(name) ((__typeof__(&name)) real_fn(#name, &real_##name))
static void *real_fn(const char *name, void **slot)
{
	if (!*slot)
		*slot = dlsym(RTLD_NEXT, name);
	return *slot;
}

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
	if (syscall(SYS_newfstatat, AT_FDCWD, "/proc/self", &st, 0, 0L, LANE) != 0)
		return;                                 /* the lane is not there: leave it all to the tracer */
	real_uid = st.st_uid;
	real_gid = st.st_gid;
	memcpy(p, di.dli_fname, n - t);
	strcpy(p + (n - t), "/var/lib/deb-native/dn-tiny.gen");
	fd = syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_CLOEXEC, 0, 0L, LANE);
	if (fd >= 0) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, (int) fd, 0);

		if (m != MAP_FAILED)
			gen = (volatile unsigned *) m;
		close((int) fd);
	}
	memcpy(root, di.dli_fname, n - t);
	root[n - t] = '\0';
	root_len = n - t;                               /* set last: until now everything falls back */
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

/* readlink of a guest path: on the host path, through the lane, from the cache */
#define LC_SLOTS 2048
struct lc { unsigned gen, hash; int r; char key[200]; char tgt[120]; };
static struct lc lcache[LC_SLOTS];

static int guest_readlink(const char *g, char *buf, size_t n)
{
	char host[PATH_MAX + PATH_MAX];
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
	if (gen && e->gen == cur && e->hash == h && strcmp(e->key, host) == 0 && (size_t) e->r <= n) {
		r = e->r;
		if (r > 0)
			memcpy(buf, e->tgt, (size_t) r);
	} else {
		r = syscall(SYS_readlinkat, AT_FDCWD, host, buf, n, 0L, LANE);
		if (r < 0)
			r = -errno;
		if (gen && len < sizeof e->key && r < (long) sizeof e->tgt) {
			e->gen = cur;
			e->hash = h;
			e->r = (int) r;
			memcpy(e->key, host, len + 1);
			if (r > 0)
				memcpy(e->tgt, buf, (size_t) r);
		}
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
static void *real_chdir, *real_fchdir;

int chdir(const char *p)
{
	cwd[0] = '\0';
	return REAL(chdir)(p);
}

int fchdir(int fd)
{
	cwd[0] = '\0';
	return REAL(fchdir)(fd);
}

/* ---- resolving ------------------------------------------------------------ */

/* glibc's realpath algorithm (stdlib/canonicalize.c) on guest paths: each component
 * resolved, links included, then `..` against what is resolved so far.  With
 * `nofollow` the last component is left as it is (a link stays a link).  0, or -errno. */
static int resolve(const char *in, int nofollow, char *res)
{
	char rest[PATH_MAX * 2], tgt[PATH_MAX];
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
		if (!cwd[0] && !getcwd(cwd, sizeof cwd))
			return -errno;
		strcpy(res, strcmp(cwd, "/") == 0 ? "" : cwd);
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
			char nrest[PATH_MAX * 2];
			size_t al = strlen(p);

			if ((size_t) r + 1 + al >= sizeof nrest)
				return -ENAMETOOLONG;
			memcpy(nrest, tgt, (size_t) r);
			nrest[r] = '/';
			memcpy(nrest + r + 1, p, al + 1);
			strcpy(rest, nrest);
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

/* the paths this file handles: absolute, or relative to the working directory */
static int ours(int dirfd, const char *path)
{
	return root_len && path && (path[0] == '/' || dirfd == AT_FDCWD);
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
	char host[PATH_MAX + PATH_MAX];
	long r;
	int e;

	if (!ours(dirfd, path) || (flags & DN_LANE_OPEN_DENY))
		return REAL(openat)(dirfd, path, flags, mode);
	e = host_of(path, flags & O_NOFOLLOW, host, sizeof host);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	r = syscall(SYS_openat, AT_FDCWD, host, flags, mode, 0L, LANE);
	return (int) r;
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
static void *real_fstatat;

/* what the program is told: the owner read as root, a link's size as readlink says */
static void fix_stat(struct stat *st, const char *res_guest, int is_lstat)
{
	if (st->st_uid == real_uid)
		st->st_uid = 0;
	if (st->st_gid == real_gid)
		st->st_gid = 0;
	if (is_lstat && S_ISLNK(st->st_mode)) {
		char tgt[PATH_MAX];
		int r = guest_readlink(res_guest, tgt, sizeof tgt - 1);

		if (r > 0)
			st->st_size = r;
	}
}

static int lane_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	char res[PATH_MAX], host[PATH_MAX + PATH_MAX];
	int nofollow = flags & AT_SYMLINK_NOFOLLOW, e;
	size_t n;

	if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0' && dirfd >= 0) {
		if (!root_len || syscall(SYS_fstat, dirfd, st, 0L, 0L, 0L, LANE) != 0)
			return root_len ? -1 : REAL(fstatat)(dirfd, path, st, flags);
		fix_stat(st, NULL, 0);
		return 0;
	}
	if (!ours(dirfd, path) || (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT)))
		return REAL(fstatat)(dirfd, path, st, flags);
	n = strlen(path);
	if (n > 0 && path[n - 1] == '/')
		nofollow = 0;
	e = resolve(path, nofollow, res);
	if (e == 0)
		e = to_host(res, host, sizeof host - 1);
	if (e == 0 && n > 0 && path[n - 1] == '/' && strcmp(res, "/") != 0)
		strcat(host, "/");
	if (e < 0) {
		errno = -e;
		return -1;
	}
	if (syscall(SYS_newfstatat, AT_FDCWD, host, st, flags & (AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT), 0L, LANE) != 0)
		return -1;
	fix_stat(st, res, nofollow);
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

/* ---- access ---------------------------------------------------------------- */
static void *real_faccessat;

static int lane_faccessat(int dirfd, const char *path, int mode, int flags)
{
	char host[PATH_MAX + PATH_MAX];
	int e;

	if (!ours(dirfd, path) || flags != 0)
		return REAL(faccessat)(dirfd, path, mode, flags);
	e = host_of(path, 0, host, sizeof host);
	if (e < 0) {
		errno = -e;
		return -1;
	}
	return (int) syscall(SYS_faccessat, AT_FDCWD, host, mode, 0L, 0L, LANE);
}

int faccessat(int dirfd, const char *path, int mode, int flags) { return lane_faccessat(dirfd, path, mode, flags); }
int access(const char *path, int mode) { return lane_faccessat(AT_FDCWD, path, mode, 0); }
