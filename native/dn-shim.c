/* dn-shim.so -- the fast path (docs/spec/dn-interp-shim.md, docs/spec/runtime.md).
 *
 * An LD_PRELOAD shared object linked with dn-policy.  dn-interp puts it in
 * the program's environment, so it is present at every dynamic glibc program,
 * however the program was launched.  For each path/identity call it
 * interposes, it translates the path through dn-policy and issues the syscall
 * from the fixed gate page (P_GATE), so the seccomp filter recognizes the call
 * as already handled and ALLOWs it.
 *
 * What it does not interpose -- libc-internal calls (NSS, gconv, ld.so),
 * inline `svc #0`, and fully static programs -- does not come from the gate
 * page, so the filter TRACEs it and dn-trace handles it.  That boundary is
 * ../docs/reference/syscall-boundary.md, not a shim gap.
 *
 * The per-function behavior mirrors main's glibc wiring
 * (patches/dn-policy-glibc-wiring.patch).  Covered so far: open, access, stat
 * (P2a/P2b-1) and the fake-root writes (P2b-2a).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "dn-policy.h"

/* The fixed gate page (mirrors src/tracer/syscall/seccomp.c and dn-interp).  */
#define DN_GATE_ADDR 0x100000000UL
#define DN_GATE_SIZE 4096

/* The "svc #0; ret" body written at P_GATE.  */
static const uint32_t dn_gate_stub[2] = { 0xd4000001u, 0xd65f03c0u };

static int dn_gate_ready;   /* gate page holds the stub (here, or dn-interp) */
static int dn_active;       /* dn_policy_init() succeeded for this prefix    */
static DnIdentity *dn_id;   /* the one identity this process fakes (P2b-2b)  */

/* Issue a syscall @nr with up to six args, from the gate page.  Freestanding
 * aarch64: move the number to x8, shift the args down to x0..x5, and branch to
 * P_GATE, whose "svc #0; ret" runs at the gate address and returns here.  */
extern long dn_gate_syscall(long nr, long a0, long a1, long a2,
			    long a3, long a4, long a5);

__asm__(
".text\n"
".globl dn_gate_syscall\n"
".type dn_gate_syscall, %function\n"
"dn_gate_syscall:\n"
"	mov	x8, x0\n"
"	mov	x0, x1\n"
"	mov	x1, x2\n"
"	mov	x2, x3\n"
"	mov	x3, x4\n"
"	mov	x4, x5\n"
"	mov	x5, x6\n"
"	movz	x9, #1, lsl #32\n"
"	br	x9\n"
".size dn_gate_syscall, .-dn_gate_syscall\n"
);

extern long dn_raw_syscall(long nr, long a0, long a1, long a2,
			   long a3, long a4, long a5);

static long dn_do_syscall(long nr, long a0, long a1, long a2, long a3, long a4)
{
	if (dn_gate_ready)
		return dn_gate_syscall(nr, a0, a1, a2, a3, a4, 0);
	return dn_raw_syscall(nr, a0, a1, a2, a3, a4, 0);
}

/* The same syscall issued directly, no gate page: the fallback when the gate
 * page could not be mapped.  Needed because syscall(2) is interposed (P2b-2g),
 * so libc's syscall() is not reachable from here without recursing.  */
__asm__(
".text\n"
".globl dn_raw_syscall\n"
".type dn_raw_syscall, %function\n"
"dn_raw_syscall:\n"
"	mov	x8, x0\n"
"	mov	x0, x1\n"
"	mov	x1, x2\n"
"	mov	x2, x3\n"
"	mov	x3, x4\n"
"	mov	x4, x5\n"
"	mov	x5, x6\n"
"	svc	#0\n"
"	ret\n"
".size dn_raw_syscall, .-dn_raw_syscall\n"
);

/* A raw syscall with up to three real args (the identity fallbacks).  */
static long dn_raw(long nr, long a0, long a1, long a2)
{
	return dn_raw_syscall(nr, a0, a1, a2, 0, 0, 0);
}

__attribute__((constructor))
static void dn_shim_init(void)
{
	const char *inst = getenv("DN_INSTDIR");
	void *p;

	p = mmap((void *)DN_GATE_ADDR, DN_GATE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == (void *)DN_GATE_ADDR) {
		memcpy(p, dn_gate_stub, sizeof(dn_gate_stub));
		mprotect(p, DN_GATE_SIZE, PROT_READ | PROT_EXEC);
		dn_gate_ready = 1;
	} else if (p != MAP_FAILED) {
		munmap(p, DN_GATE_SIZE);           /* never on NOREPLACE */
	} else if (errno == EEXIST) {
		dn_gate_ready = 1;                 /* dn-interp already mapped it */
	}

	if (inst != NULL) {
		char rt[4096];
		if ((size_t)snprintf(rt, sizeof(rt), "%s/usr/lib/deb-native", inst) < sizeof(rt)
		    && dn_policy_init(inst, rt) == 0) {
			dn_id = dn_policy_identity_new();
			dn_active = (dn_id != NULL);
		}
	}
}

/* dn_policy_redirect()'s semantics (dn-policy-glue.c): translate an absolute
 * guest path into @buf and return @buf, or return @path unchanged when the
 * policy is off or declines (relative, too long, ...).  @follow=0 for a call
 * that must not resolve the final component (lstat, AT_SYMLINK_NOFOLLOW).  */
static const char *dn_redirect(char *buf, size_t cap, const char *path, int follow)
{
	int status;

	if (!dn_active || path == NULL || path[0] != '/')
		return path;
	status = follow ? dn_policy_translate_path(path, buf, cap)
			: dn_policy_translate_path_nofollow(path, buf, cap);
	return status < 0 ? path : buf;
}

/* 0/-1 with errno, from a raw syscall result.  */
static int dn_ret(long r)
{
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}

static ssize_t dn_ret_ssize(long r)
{
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (ssize_t)r;
}

/* dn_policy_stat_post(): rewrite a stat result's owner fields from the owner
 * store, only when the path was actually translated (mirrors fstatat64.c).  */
static void dn_stat_post(const char *host, struct stat *st)
{
	if (dn_active)
		dn_policy_fake_stat(host, st);
}

/* stat @host into @st with a raw syscall (no fake-root post): @follow for the
 * dev/ino the owner store keys on.  Returns 0 or -1.  */
static int dn_stat_host(const char *host, struct stat *st, int follow)
{
	long r = dn_do_syscall(SYS_newfstatat, AT_FDCWD, (long)host, (long)st,
			       follow ? 0 : AT_SYMLINK_NOFOLLOW, 0);
	return dn_ret(r);
}

/* ---- open family (P2a) ---------------------------------------------- */

int openat(int dirfd, const char *path, int flags, ...)
{
	char buf[PATH_MAX];
	const char *p;
	mode_t mode = 0;
	long r;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	p = dn_redirect(buf, sizeof(buf), path, 1);
	r = dn_do_syscall(SYS_openat, dirfd, (long)p, flags, mode, 0);
	return dn_ret(r);
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return openat(AT_FDCWD, path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return openat(AT_FDCWD, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return openat(dirfd, path, flags, mode);
}

/* ---- access family -------------------------------------------------- */

int access(const char *path, int mode)
{
	char buf[PATH_MAX];
	const char *p = dn_redirect(buf, sizeof(buf), path, 1);

	return dn_ret(dn_do_syscall(SYS_faccessat, AT_FDCWD, (long)p, mode, 0, 0));
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
	char buf[PATH_MAX];
	const char *p = dn_redirect(buf, sizeof(buf), path, !(flags & AT_SYMLINK_NOFOLLOW));

	return dn_ret(dn_do_syscall(SYS_faccessat, dirfd, (long)p, mode, flags, 0));
}

/* ---- stat family (P2b-1) -------------------------------------------- */

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
	char pb[PATH_MAX];
	int follow = !(flags & AT_SYMLINK_NOFOLLOW);
	const char *p = dn_redirect(pb, sizeof(pb), path, follow);
	long r = dn_do_syscall(SYS_newfstatat, dirfd, (long)p, (long)buf, flags, 0);

	if (r == 0 && p != path)
		dn_stat_post(p, buf);
	return dn_ret(r);
}

int stat(const char *path, struct stat *buf)
{
	return fstatat(AT_FDCWD, path, buf, 0);
}

int lstat(const char *path, struct stat *buf)
{
	return fstatat(AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}

int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags)
{
	return fstatat(dirfd, path, (struct stat *)buf, flags);
}

int stat64(const char *path, struct stat64 *buf)
{
	return fstatat(AT_FDCWD, path, (struct stat *)buf, 0);
}

int lstat64(const char *path, struct stat64 *buf)
{
	return fstatat(AT_FDCWD, path, (struct stat *)buf, AT_SYMLINK_NOFOLLOW);
}

/* ---- fake-root writes (P2b-2a) -------------------------------------- *
 * chown/lchown never touch the real file: they record the requested owner in
 * the store, keyed on (dev, ino).  chmod/fchmodat put the ordinary bits on
 * the real file and record only the setuid/setgid bits (Android would drop
 * them).  All mirror chown.c/lchown.c/chmod.c/fchmodat.c of the wiring.  */

int chown(const char *path, uid_t owner, gid_t group)
{
	char buf[PATH_MAX];
	const char *host;
	struct stat st;

	if (!dn_active)
		return dn_ret(dn_do_syscall(SYS_fchownat, AT_FDCWD, (long)path,
					    owner, group, 0));
	host = dn_redirect(buf, sizeof(buf), path, 1);
	if (dn_stat_host(host, &st, 1) != 0)
		return -1;
	if (dn_policy_owner_merge(host, (uint64_t)st.st_dev, (uint64_t)st.st_ino,
				  1, (uint32_t)owner, (uint32_t)group, 0, 0) < 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int lchown(const char *path, uid_t owner, gid_t group)
{
	char buf[PATH_MAX];
	const char *host;
	struct stat st;

	if (!dn_active)
		return dn_ret(dn_do_syscall(SYS_fchownat, AT_FDCWD, (long)path,
					    owner, group, AT_SYMLINK_NOFOLLOW));
	host = dn_redirect(buf, sizeof(buf), path, 0);
	if (dn_stat_host(host, &st, 0) != 0)
		return -1;
	if (dn_policy_owner_merge(host, (uint64_t)st.st_dev, (uint64_t)st.st_ino,
				  1, (uint32_t)owner, (uint32_t)group, 0, 0) < 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int chmod(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	const char *host;
	struct stat st;

	if (!dn_active)
		return dn_ret(dn_do_syscall(SYS_fchmodat, AT_FDCWD, (long)path, mode, 0, 0));
	host = dn_redirect(buf, sizeof(buf), path, 1);
	if (dn_stat_host(host, &st, 1) != 0)
		return -1;
	if (dn_ret(dn_do_syscall(SYS_fchmodat, AT_FDCWD, (long)host,
				 mode & ~(mode_t)07000, 0, 0)) != 0)
		return -1;
	if (dn_policy_owner_merge(host, (uint64_t)st.st_dev, (uint64_t)st.st_ino,
				  0, 0, 0, 1, (uint32_t)mode) < 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
	char buf[PATH_MAX];
	const char *host;
	struct stat st;

	if (dn_active && (path[0] == '/' || dirfd == AT_FDCWD)) {
		host = dn_redirect(buf, sizeof(buf), path, 1);
		if (dn_stat_host(host, &st, 1) != 0)
			return -1;
		if (dn_ret(dn_do_syscall(SYS_fchmodat, AT_FDCWD, (long)host,
					 mode & ~(mode_t)07000, 0, 0)) != 0)
			return -1;
		if (dn_policy_owner_merge(host, (uint64_t)st.st_dev, (uint64_t)st.st_ino,
					  0, 0, 0, 1, (uint32_t)mode) < 0) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
	return dn_ret(dn_do_syscall(SYS_fchmodat, dirfd, (long)path, mode, flags, 0));
}

/* ---- identity (P2b-2b) --------------------------------------------- *
 * Fake root.  The get*id()/set*id() family answers from the one static
 * DnIdentity (dn-policy.h: the shim runs in-process, so it tracks one
 * program); the glibc wiring left this to the tracer, and the shim is where
 * it belongs.  With dn-policy off, the real ids pass through.  */

uid_t getuid(void)
{
	return dn_id ? dn_policy_fake_getuid(dn_id) : (uid_t)dn_raw(SYS_getuid, 0, 0, 0);
}

uid_t geteuid(void)
{
	return dn_id ? dn_policy_fake_getuid(dn_id) : (uid_t)dn_raw(SYS_geteuid, 0, 0, 0);
}

gid_t getgid(void)
{
	return dn_id ? dn_policy_fake_getgid(dn_id) : (gid_t)dn_raw(SYS_getgid, 0, 0, 0);
}

gid_t getegid(void)
{
	return dn_id ? dn_policy_fake_getgid(dn_id) : (gid_t)dn_raw(SYS_getegid, 0, 0, 0);
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	if (dn_id) {
		dn_policy_fake_getresuid(dn_id, ruid, euid, suid);
		return 0;
	}
	return dn_ret(dn_raw(SYS_getresuid, (long)ruid, (long)euid, (long)suid));
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	if (dn_id) {
		dn_policy_fake_getresgid(dn_id, rgid, egid, sgid);
		return 0;
	}
	return dn_ret(dn_raw(SYS_getresgid, (long)rgid, (long)egid, (long)sgid));
}

int getgroups(int size, gid_t list[])
{
	if (dn_id)
		return dn_policy_fake_getgroups(dn_id, (size_t)size, list);
	return dn_ret(dn_raw(SYS_getgroups, size, (long)list, 0));
}

int setuid(uid_t uid)
{
	return dn_ret(dn_id ? dn_policy_fake_setuid(dn_id, uid) : dn_raw(SYS_setuid, uid, 0, 0));
}

int setgid(gid_t gid)
{
	return dn_ret(dn_id ? dn_policy_fake_setgid(dn_id, gid) : dn_raw(SYS_setgid, gid, 0, 0));
}

int setreuid(uid_t r, uid_t e)
{
	return dn_ret(dn_id ? dn_policy_fake_setreuid(dn_id, r, e) : dn_raw(SYS_setreuid, r, e, 0));
}

int setregid(gid_t r, gid_t e)
{
	return dn_ret(dn_id ? dn_policy_fake_setregid(dn_id, r, e) : dn_raw(SYS_setregid, r, e, 0));
}

int setresuid(uid_t r, uid_t e, uid_t s)
{
	return dn_ret(dn_id ? dn_policy_fake_setresuid(dn_id, r, e, s)
			     : dn_raw(SYS_setresuid, r, e, s));
}

int setresgid(gid_t r, gid_t e, gid_t s)
{
	return dn_ret(dn_id ? dn_policy_fake_setresgid(dn_id, r, e, s)
			     : dn_raw(SYS_setresgid, r, e, s));
}

uid_t setfsuid(uid_t uid)
{
	return dn_id ? dn_policy_fake_setfsuid(dn_id, uid)
		     : (uid_t)dn_raw(SYS_setfsuid, uid, 0, 0);
}

gid_t setfsgid(gid_t gid)
{
	return dn_id ? dn_policy_fake_setfsgid(dn_id, gid)
		     : (gid_t)dn_raw(SYS_setfsgid, gid, 0, 0);
}

int setgroups(size_t n, const gid_t *list)
{
	return dn_ret(dn_id ? dn_policy_fake_setgroups(dn_id, n, list)
			     : dn_raw(SYS_setgroups, n, (long)list, 0));
}

/* ---- path manipulation (P2b-2c) ------------------------------------ *
 * The wiring's mkdir/rmdir/rename{,at,at2}/symlink/truncate/utimensat/
 * statfs helpers, all redirect_nofollow.  rename translates both paths;
 * symlink only the link path (the target text is stored verbatim).  An
 * absolute path goes to AT_FDCWD with its host form; a relative one keeps
 * its dirfd (dn_redirect leaves it untouched).  chdir stays on the tracer so
 * the traced path and the shim agree on the real cwd.  */

int mkdirat(int dirfd, const char *path, mode_t mode)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret(dn_do_syscall(SYS_mkdirat, h != path ? AT_FDCWD : dirfd, (long)h, mode, 0, 0));
}

int mkdir(const char *path, mode_t mode)
{
	return mkdirat(AT_FDCWD, path, mode);
}

int unlinkat(int dirfd, const char *path, int flags)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	if (dn_active)
		(void)dn_policy_unlink(h);
	return dn_ret(dn_do_syscall(SYS_unlinkat, h != path ? AT_FDCWD : dirfd, (long)h, flags, 0, 0));
}

int unlink(const char *path)
{
	return unlinkat(AT_FDCWD, path, 0);
}

int rmdir(const char *path)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret(dn_do_syscall(SYS_unlinkat, AT_FDCWD, (long)h, AT_REMOVEDIR, 0, 0));
}

int renameat(int olddirfd, const char *old, int newdirfd, const char *new)
{
	char b1[PATH_MAX], b2[PATH_MAX];
	const char *ho = dn_redirect(b1, sizeof(b1), old, 0);
	const char *hn = dn_redirect(b2, sizeof(b2), new, 0);

	return dn_ret(dn_do_syscall(SYS_renameat, ho != old ? AT_FDCWD : olddirfd, (long)ho,
				    hn != new ? AT_FDCWD : newdirfd, (long)hn, 0));
}

int rename(const char *old, const char *new)
{
	return renameat(AT_FDCWD, old, AT_FDCWD, new);
}

int renameat2(int olddirfd, const char *old, int newdirfd, const char *new, unsigned int flags)
{
	char b1[PATH_MAX], b2[PATH_MAX];
	const char *ho = dn_redirect(b1, sizeof(b1), old, 0);
	const char *hn = dn_redirect(b2, sizeof(b2), new, 0);

	return dn_ret(dn_do_syscall(SYS_renameat2, ho != old ? AT_FDCWD : olddirfd, (long)ho,
				    hn != new ? AT_FDCWD : newdirfd, (long)hn, flags));
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), linkpath, 0);

	return dn_ret(dn_do_syscall(SYS_symlinkat, (long)target,
				    h != linkpath ? AT_FDCWD : newdirfd, (long)h, 0, 0));
}

int symlink(const char *target, const char *linkpath)
{
	return symlinkat(target, AT_FDCWD, linkpath);
}

int truncate(const char *path, off_t length)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret(dn_do_syscall(SYS_truncate, (long)h, length, 0, 0, 0));
}

int truncate64(const char *path, off64_t length)
{
	return truncate(path, (off_t)length);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret(dn_do_syscall(SYS_utimensat, h != path ? AT_FDCWD : dirfd,
				    (long)h, (long)times, flags, 0));
}

int utimes(const char *path, const struct timeval tv[2])
{
	struct timespec ts[2];

	if (tv != NULL) {
		ts[0].tv_sec = tv[0].tv_sec;
		ts[0].tv_nsec = tv[0].tv_usec * 1000;
		ts[1].tv_sec = tv[1].tv_sec;
		ts[1].tv_nsec = tv[1].tv_usec * 1000;
	}
	return utimensat(AT_FDCWD, path, tv != NULL ? ts : NULL, 0);
}

int utime(const char *path, const struct utimbuf *t)
{
	struct timespec ts[2];

	if (t != NULL) {
		ts[0].tv_sec = t->actime;
		ts[0].tv_nsec = 0;
		ts[1].tv_sec = t->modtime;
		ts[1].tv_nsec = 0;
	}
	return utimensat(AT_FDCWD, path, t != NULL ? ts : NULL, 0);
}

int statfs(const char *path, struct statfs *buf)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret(dn_do_syscall(SYS_statfs, (long)h, (long)buf, 0, 0, 0));
}

int statfs64(const char *path, struct statfs64 *buf)
{
	return statfs(path, (struct statfs *)buf);
}

/* ---- hardlinks (P2b-2d) -------------------------------------------- *
 * Android blocks link() in app data (SELinux), so this is link2symlink:
 * dn-policy moves the content into a hidden file and both names become
 * symlinks to it.  unlink's bookkeeping runs before the real unlink (see
 * unlinkat() above).  */

int link(const char *from, const char *to)
{
	char b1[PATH_MAX], b2[PATH_MAX];

	if (dn_active) {
		const char *hf = dn_redirect(b1, sizeof(b1), from, 0);
		const char *ht = dn_redirect(b2, sizeof(b2), to, 0);
		int s = dn_policy_link(hf, ht);

		if (s < 0) {
			errno = -s;
			return -1;
		}
		return 0;
	}
	return dn_ret(dn_do_syscall(SYS_linkat, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0));
}

int linkat(int olddirfd, const char *old, int newdirfd, const char *new, int flags)
{
	char b1[PATH_MAX], b2[PATH_MAX];

	if (dn_active) {
		const char *ho = dn_redirect(b1, sizeof(b1), old, 0);
		const char *hn = dn_redirect(b2, sizeof(b2), new, 0);
		int s = dn_policy_link(ho, hn);

		if (s < 0) {
			errno = -s;
			return -1;
		}
		return 0;
	}
	return dn_ret(dn_do_syscall(SYS_linkat, olddirfd, (long)old, newdirfd, (long)new, flags));
}

/* ---- xattr family (P2b-2e) ----------------------------------------- *
 * security.* (setcap and friends) is always faked -- a real write is what
 * Android denies -- so route it to dn_policy_fake_*xattr(), keyed like the
 * owner record.  Everything else is the real xattr, path-translated.  The
 * l* variants do not follow a final symlink.  */

int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 1);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		if (dn_stat_host(h, &st, 1) != 0)
			return -1;
		return dn_ret(dn_policy_fake_setxattr(h, st.st_dev, st.st_ino, name, value, size));
	}
	return dn_ret(dn_do_syscall(SYS_setxattr, (long)h, (long)name, (long)value, size, flags));
}

int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		if (dn_stat_host(h, &st, 0) != 0)
			return -1;
		return dn_ret(dn_policy_fake_setxattr(h, st.st_dev, st.st_ino, name, value, size));
	}
	return dn_ret(dn_do_syscall(SYS_lsetxattr, (long)h, (long)name, (long)value, size, flags));
}

ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 1);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		int r;
		if (dn_stat_host(h, &st, 1) != 0)
			return -1;
		r = dn_policy_fake_getxattr(h, st.st_dev, st.st_ino, name, value, size);
		if (r < 0) {
			errno = -r;
			return -1;
		}
		return r;
	}
	return dn_ret_ssize(dn_do_syscall(SYS_getxattr, (long)h, (long)name, (long)value, size, 0));
}

ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		int r;
		if (dn_stat_host(h, &st, 0) != 0)
			return -1;
		r = dn_policy_fake_getxattr(h, st.st_dev, st.st_ino, name, value, size);
		if (r < 0) {
			errno = -r;
			return -1;
		}
		return r;
	}
	return dn_ret_ssize(dn_do_syscall(SYS_lgetxattr, (long)h, (long)name, (long)value, size, 0));
}

int removexattr(const char *path, const char *name)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 1);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		if (dn_stat_host(h, &st, 1) != 0)
			return -1;
		return dn_ret(dn_policy_fake_removexattr(h, st.st_dev, st.st_ino, name));
	}
	return dn_ret(dn_do_syscall(SYS_removexattr, (long)h, (long)name, 0, 0, 0));
}

int lremovexattr(const char *path, const char *name)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	if (dn_active && dn_policy_is_fake_xattr(name)) {
		struct stat st;
		if (dn_stat_host(h, &st, 0) != 0)
			return -1;
		return dn_ret(dn_policy_fake_removexattr(h, st.st_dev, st.st_ino, name));
	}
	return dn_ret(dn_do_syscall(SYS_lremovexattr, (long)h, (long)name, 0, 0, 0));
}

ssize_t listxattr(const char *path, char *list, size_t size)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 1);

	return dn_ret_ssize(dn_do_syscall(SYS_listxattr, (long)h, (long)list, size, 0, 0));
}

ssize_t llistxattr(const char *path, char *list, size_t size)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);

	return dn_ret_ssize(dn_do_syscall(SYS_llistxattr, (long)h, (long)list, size, 0, 0));
}

/* ---- reverse translation (P2b-2f) ---------------------------------- *
 * getcwd() and readlink() hand back a real host path; the guest must see the
 * tree path.  Mirrors dn_policy_getcwd_post()/dn_policy_readlink_post().  */

static void dn_detranslate_inplace(char *buf, size_t cap)
{
	char out[PATH_MAX];
	size_t n;

	if (!dn_active || buf == NULL)
		return;
	if (dn_policy_detranslate_path(buf, out, sizeof(out)) != 0)
		return;
	n = strlen(out);
	if (n < cap)
		memcpy(buf, out, n + 1);
}

char *getcwd(char *buf, size_t size)
{
	long r;

	if (buf == NULL) {
		size = size ? size : PATH_MAX;
		buf = malloc(size);
		if (buf == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	}
	r = dn_do_syscall(SYS_getcwd, (long)buf, size, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return NULL;
	}
	dn_detranslate_inplace(buf, size);
	return buf;
}

ssize_t readlink(const char *path, char *buf, size_t len)
{
	return readlinkat(AT_FDCWD, path, buf, len);
}

/* Answer a /proc/<self> magic link: /proc/self/exe with the recorded program
 * (DN_PROG, set by dn-interp), everything else with reverse translation.  */
static ssize_t dn_readlink_post(const char *path, char *buf, size_t len, long n)
{
	char in[PATH_MAX], out[PATH_MAX];
	const char *prog;
	size_t olen;

	if (!dn_active || path == NULL || strncmp(path, "/proc/", 6) != 0)
		return n;

	if (strcmp(path, "/proc/self/exe") == 0 || strcmp(path, "/proc/thread-self/exe") == 0) {
		prog = getenv("DN_PROG");
		if (prog != NULL && dn_policy_detranslate_path(prog, out, sizeof(out)) == 0) {
			olen = strlen(out);
			if (olen <= len) {
				memcpy(buf, out, olen);
				return (ssize_t)olen;
			}
		}
		return n;
	}

	if ((size_t)n >= sizeof(in))
		return n;
	memcpy(in, buf, (size_t)n);
	in[n] = '\0';
	if (dn_policy_detranslate_path(in, out, sizeof(out)) != 0)
		return n;
	olen = strlen(out);
	if (olen > len)
		return n;
	memcpy(buf, out, olen);
	return (ssize_t)olen;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t len)
{
	char b[PATH_MAX];
	const char *h = dn_redirect(b, sizeof(b), path, 0);
	long n = dn_do_syscall(SYS_readlinkat, dirfd, (long)h, (long)buf, len, 0);

	if (n < 0)
		return dn_ret_ssize(n);
	return dn_readlink_post(path, buf, len, n);
}

/* ---- syscall(2) (P2b-2g) ------------------------------------------- *
 * A program that reaches the kernel through syscall(2) bypasses every
 * wrapper above, so translate the path argument(s) of a path-group number
 * here and issue the call from the gate -- mirroring the wiring's
 * dn_policy_syscall_args().  */

static void dn_sysarg_path(long *arg, int follow, char *buf, size_t cap)
{
	const char *path = (const char *)(uintptr_t)*arg;
	const char *out;

	if (path == NULL || path[0] != '/')
		return;
	out = dn_redirect(buf, cap, path, follow);
	*arg = (long)(uintptr_t)out;
}

static void dn_syscall_args(long nr, long *a0, long *a1, long *a2, long *a3,
			    char *p0, size_t cap, char *p1)
{
	switch (nr) {
	case SYS_openat:
		dn_sysarg_path(a1, !((int)*a2 & O_NOFOLLOW), p0, cap);
		break;
	case SYS_newfstatat:
		dn_sysarg_path(a1, !((int)*a3 & AT_SYMLINK_NOFOLLOW), p0, cap);
		break;
#ifdef SYS_statx
	case SYS_statx:
		dn_sysarg_path(a1, !((int)*a2 & AT_SYMLINK_NOFOLLOW), p0, cap);
		break;
#endif
	case SYS_faccessat:
		dn_sysarg_path(a1, 1, p0, cap);
		break;
#ifdef SYS_faccessat2
	case SYS_faccessat2:
		dn_sysarg_path(a1, !((int)*a3 & AT_SYMLINK_NOFOLLOW), p0, cap);
		break;
#endif
	case SYS_readlinkat:
		dn_sysarg_path(a1, 0, p0, cap);
		break;
#ifdef SYS_openat2
	case SYS_openat2:
		dn_sysarg_path(a1, 1, p0, cap);
		break;
#endif
	case SYS_mkdirat:
	case SYS_mknodat:
	case SYS_unlinkat:
	case SYS_fchmodat:
#ifdef SYS_fchmodat2
	case SYS_fchmodat2:
#endif
	case SYS_utimensat:
	case SYS_fchownat:
		dn_sysarg_path(a1, 0, p0, cap);
		break;
	case SYS_symlinkat:
		dn_sysarg_path(a2, 0, p0, cap);
		break;
	case SYS_renameat:
#ifdef SYS_renameat2
	case SYS_renameat2:
#endif
	case SYS_linkat:
		dn_sysarg_path(a1, 0, p0, cap);
		dn_sysarg_path(a3, 0, p1, cap);
		break;
#ifdef SYS_chown
	case SYS_chown:
#endif
#ifdef SYS_lchown
	case SYS_lchown:
#endif
	case SYS_chdir:
	case SYS_chroot:
	case SYS_truncate:
	case SYS_statfs:
	case SYS_setxattr:
	case SYS_lsetxattr:
	case SYS_getxattr:
	case SYS_lgetxattr:
	case SYS_listxattr:
	case SYS_llistxattr:
	case SYS_removexattr:
	case SYS_lremovexattr:
		dn_sysarg_path(a0, 0, p0, cap);
		break;
	default:
		break;
	}
}

long syscall(long number, ...)
{
	va_list ap;
	long a0, a1, a2, a3, a4, a5, r;
	char p0[PATH_MAX], p1[PATH_MAX];

	va_start(ap, number);
	a0 = va_arg(ap, long);
	a1 = va_arg(ap, long);
	a2 = va_arg(ap, long);
	a3 = va_arg(ap, long);
	a4 = va_arg(ap, long);
	a5 = va_arg(ap, long);
	va_end(ap);

	if (dn_active)
		dn_syscall_args(number, &a0, &a1, &a2, &a3, p0, sizeof(p0), p1);

	r = dn_gate_ready ? dn_gate_syscall(number, a0, a1, a2, a3, a4, a5)
			  : dn_raw_syscall(number, a0, a1, a2, a3, a4, a5);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return r;
}
