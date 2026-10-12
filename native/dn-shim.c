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
 * (patches/dn-policy-glibc-wiring.patch): open/access -> redirect; the stat
 * family -> redirect + dn_policy_fake_stat(); and so on.  P2a/P2b-1 cover the
 * open, access and stat families; the rest is P2b-2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

/* Set once the gate page holds the stub (mapped here, or already by dn-interp).  */
static int dn_gate_ready;

/* Set once dn_policy_init() succeeded for this process's prefix.  */
static int dn_active;

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

static long dn_do_syscall(long nr, long a0, long a1, long a2, long a3)
{
	if (dn_gate_ready)
		return dn_gate_syscall(nr, a0, a1, a2, a3, 0, 0);
	return syscall(nr, a0, a1, a2, a3);
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
		    && dn_policy_init(inst, rt) == 0)
			dn_active = 1;
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
	r = dn_do_syscall(SYS_openat, dirfd, (long)p, flags, mode);
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

	return dn_ret(dn_do_syscall(SYS_faccessat, AT_FDCWD, (long)p, mode, 0));
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
	char buf[PATH_MAX];
	const char *p = dn_redirect(buf, sizeof(buf), path, !(flags & AT_SYMLINK_NOFOLLOW));

	return dn_ret(dn_do_syscall(SYS_faccessat, dirfd, (long)p, mode, flags));
}

/* ---- stat family ---------------------------------------------------- */

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
	char pb[PATH_MAX];
	int follow = !(flags & AT_SYMLINK_NOFOLLOW);
	const char *p = dn_redirect(pb, sizeof(pb), path, follow);
	long r = dn_do_syscall(SYS_newfstatat, dirfd, (long)p, (long)buf, flags);

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

/* The 64-bit aliases (LP64: same layout as stat on aarch64).  */
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
