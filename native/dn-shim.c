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
#include <sys/types.h>
#include <unistd.h>

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

static long dn_do_syscall(long nr, long a0, long a1, long a2, long a3, long a4)
{
	if (dn_gate_ready)
		return dn_gate_syscall(nr, a0, a1, a2, a3, a4, 0);
	return syscall(nr, a0, a1, a2, a3, a4);
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
	return dn_id ? dn_policy_fake_getuid(dn_id) : (uid_t)syscall(SYS_getuid);
}

uid_t geteuid(void)
{
	return dn_id ? dn_policy_fake_getuid(dn_id) : (uid_t)syscall(SYS_geteuid);
}

gid_t getgid(void)
{
	return dn_id ? dn_policy_fake_getgid(dn_id) : (gid_t)syscall(SYS_getgid);
}

gid_t getegid(void)
{
	return dn_id ? dn_policy_fake_getgid(dn_id) : (gid_t)syscall(SYS_getegid);
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	if (dn_id) {
		dn_policy_fake_getresuid(dn_id, ruid, euid, suid);
		return 0;
	}
	return dn_ret(syscall(SYS_getresuid, ruid, euid, suid));
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	if (dn_id) {
		dn_policy_fake_getresgid(dn_id, rgid, egid, sgid);
		return 0;
	}
	return dn_ret(syscall(SYS_getresgid, rgid, egid, sgid));
}

int getgroups(int size, gid_t list[])
{
	if (dn_id)
		return dn_policy_fake_getgroups(dn_id, (size_t)size, list);
	return dn_ret(syscall(SYS_getgroups, size, list));
}

int setuid(uid_t uid)
{
	return dn_ret(dn_id ? dn_policy_fake_setuid(dn_id, uid) : syscall(SYS_setuid, uid));
}

int setgid(gid_t gid)
{
	return dn_ret(dn_id ? dn_policy_fake_setgid(dn_id, gid) : syscall(SYS_setgid, gid));
}

int setreuid(uid_t r, uid_t e)
{
	return dn_ret(dn_id ? dn_policy_fake_setreuid(dn_id, r, e)
			     : syscall(SYS_setreuid, r, e));
}

int setregid(gid_t r, gid_t e)
{
	return dn_ret(dn_id ? dn_policy_fake_setregid(dn_id, r, e)
			     : syscall(SYS_setregid, r, e));
}

int setresuid(uid_t r, uid_t e, uid_t s)
{
	return dn_ret(dn_id ? dn_policy_fake_setresuid(dn_id, r, e, s)
			     : syscall(SYS_setresuid, r, e, s));
}

int setresgid(gid_t r, gid_t e, gid_t s)
{
	return dn_ret(dn_id ? dn_policy_fake_setresgid(dn_id, r, e, s)
			     : syscall(SYS_setresgid, r, e, s));
}

uid_t setfsuid(uid_t uid)
{
	return dn_id ? dn_policy_fake_setfsuid(dn_id, uid)
		     : (uid_t)syscall(SYS_setfsuid, uid);
}

gid_t setfsgid(gid_t gid)
{
	return dn_id ? dn_policy_fake_setfsgid(dn_id, gid)
		     : (gid_t)syscall(SYS_setfsgid, gid);
}

int setgroups(size_t n, const gid_t *list)
{
	return dn_ret(dn_id ? dn_policy_fake_setgroups(dn_id, n, list)
			     : syscall(SYS_setgroups, n, list));
}
