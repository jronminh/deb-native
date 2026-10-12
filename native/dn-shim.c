/* dn-shim.so -- the fast path (docs/spec/ld-dn-shim.md, docs/spec/overlay.md).
 *
 * An LD_PRELOAD shared object linked with dn-policy.  ld-dn puts it in the
 * program's environment, so it is present at every dynamic glibc program,
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
 * P2a: the open family only; the rest of the surface is P2b.
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
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "dn-policy.h"

/* The fixed gate page (mirrors src/tracer/syscall/seccomp.c and ld-dn).  */
#define DN_GATE_ADDR 0x100000000UL
#define DN_GATE_SIZE 4096

/* The "svc #0; ret" body written at P_GATE.  */
static const uint32_t dn_gate_stub[2] = { 0xd4000001u, 0xd65f03c0u };

/* Set once the gate page holds the stub (mapped here, or already by ld-dn).  */
static int dn_gate_ready;

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

static long dn_do_openat(long dirfd, const char *path, long flags, long mode)
{
	if (dn_gate_ready)
		return dn_gate_syscall(SYS_openat, dirfd, (long)path, flags, mode, 0, 0);
	return syscall(SYS_openat, dirfd, path, flags, mode);
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
		dn_gate_ready = 1;                 /* ld-dn already mapped it */
	}

	if (inst != NULL) {
		char rt[4096];
		if ((size_t)snprintf(rt, sizeof(rt), "%s/usr/lib/deb-native", inst) < sizeof(rt))
			(void)dn_policy_init(inst, rt);
	}
}

/* Translate an absolute guest path; leave a relative one to the kernel.  */
static const char *dn_path(const char *path, char *buf, size_t cap)
{
	if (path == NULL || path[0] != '/')
		return path;
	if (dn_policy_translate_path(path, buf, cap) != 0)
		return path;
	return buf;
}

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

	p = dn_path(path, buf, sizeof(buf));
	r = dn_do_openat(dirfd, p, flags, mode);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
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
