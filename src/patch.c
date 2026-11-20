/* patch.c -- patch the svc sites of an image dn-exec maps (the loader, a static
 * program), so the syscalls made from that image are mapped through the one law
 * and re-issued on the lane -- no tracer stop.  One mechanism, whichever code
 * location the svc sits in.  Freestanding, linked with the law (law.c). */
#include "law.h"
#include "patch.h"
#include <stdint.h>

#define COOKIE 0x9e1ab6c3d7f2a541UL

#define SYS_mmap 222
#define SYS_mprotect 226
#define SYS_munmap 215

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

#define FRAME 272
#define PBUF  512
#define SITE  (FRAME + PBUF)

/* the syscalls the hook answers, and the register each keeps its path in: map
 * the path and re-issue the call on the lane.  Only calls whose result needs no
 * fix-up are listed; the stat/readlink/getcwd/xattr family needs the owner-store
 * and link-copy logic, which the shared law should own, not a third copy here. */
static int hook_path_reg(unsigned n)
{
	switch (n) {
	case 56: case 48:               /* openat, faccessat: path in x1 */
		return 1;
	case 43: case 45:               /* statfs, truncate: path in x0 */
		return 0;
	default:
		return -1;
	}
}

/* The hook: map the path through the one law and mark the call with the lane
 * cookie, so the filter lets the already-mapped call through without a stop.
 * A write open, or a relative path (dirfd resolution), is left untouched: the
 * tracer handles it. */
static void exec_hook(unsigned long *r)
{
	long nr = (long) r[8];
	int pr = hook_path_reg((unsigned) nr);

	if (pr < 0)
		return;
	if (nr == 56 && (r[2] & 0x643))        /* O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND */
		return;
	const char *path = (const char *) r[pr];

	if (path && path[0] == '/') {
		char *host = (char *) r + FRAME;
		int n = policy_path_nf(path, host, PBUF, 0);

		if (n > 0) {
			r[pr] = (unsigned long) host;
			r[5] = (unsigned long) COOKIE;
		}
	}
}

static unsigned b_enc(void *from, void *to) { long d = ((char *) to - (char *) from) >> 2; return 0x14000000u | ((unsigned) d & 0x3ffffffu); }
static unsigned sub_sp(unsigned imm) { return 0xd1000000u | ((imm & 0xfffu) << 10) | (31u << 5) | 31u; }
static unsigned add_sp(unsigned imm) { return 0x91000000u | ((imm & 0xfffu) << 10) | (31u << 5) | 31u; }
static unsigned stp(int a, int b, int off) { return 0xa9000000u | ((unsigned) (off / 8) & 0x7fu) << 15 | (unsigned) b << 10 | 31u << 5 | (unsigned) a; }
static unsigned ldp(int a, int b, int off) { return 0xa9400000u | ((unsigned) (off / 8) & 0x7fu) << 15 | (unsigned) b << 10 | 31u << 5 | (unsigned) a; }
static unsigned strx(int t, int off) { return 0xf9000000u | ((unsigned) (off / 8) << 10) | (31u << 5) | (unsigned) t; }
static unsigned ldrx(int t, int off) { return 0xf9400000u | ((unsigned) (off / 8) << 10) | (31u << 5) | (unsigned) t; }
static unsigned branch(unsigned op, const void *from, const void *to) { long d = ((const char *) to - (const char *) from) / 4; return op | ((unsigned) d & 0x3ffffffu); }

static void flush(void *lo, void *hi)
{
	__asm__ volatile("dsb ish" ::: "memory");
	for (char *p = (char *) ((unsigned long) lo & ~63UL); p < (char *) hi; p += 64)
		__asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	for (char *p = (char *) ((unsigned long) lo & ~63UL); p < (char *) hi; p += 64)
		__asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}

static unsigned *emit_p;
static void emit(unsigned i) { *emit_p++ = i; }

/* The shared block: save x0..x28, nzcv and x30; call exec_hook; restore; make the
 * call.  The per-site stubs branch here. */
static void emit_common(void)
{
	unsigned *lit;
	int i;

	for (i = 0; i < 28; i += 2)
		emit(stp(i, i + 1, i * 8));
	emit(strx(28, 224));
	emit(0xd53b4209u);                          /* mrs x9, nzcv */
	emit(strx(9, 248));
	emit(strx(30, 256));
	emit(0x910003e0u);                          /* mov x0, sp */
	lit = emit_p;
	emit(0);                                    /* ldr x16, =exec_hook */
	emit(0xd63f0200u);                          /* blr x16 */
	emit(ldrx(9, 248));
	emit(0xd51b4209u);                          /* msr nzcv, x9 */
	emit(ldrx(30, 256));
	for (i = 0; i < 28; i += 2)
		emit(ldp(i, i + 1, i * 8));
	emit(ldrx(28, 224));
	emit(0xd4000001u);                          /* svc #0 */
	emit(0xaa1f03e5u);                          /* mov x5, xzr */
	emit(0xd65f03c0u);                          /* ret */
	if ((unsigned long) emit_p & 4)
		emit(0xd503201fu);                      /* nop: the literal is 8-aligned */
	*lit = 0x58000000u | ((unsigned) (emit_p - lit) & 0x7ffff) << 5 | 16u;
	*(void **) emit_p = (void *) exec_hook;
	emit_p += 2;
}

/* Patch the path syscalls' `mov x8,#nr; svc #0` sites in [lo, hi).  The same
 * function maps a static program and the loader: it is the one fast-path
 * mechanism for whatever dn-exec maps. */
void patch_image(uint64_t lo, uint64_t hi)
{
	unsigned *sites[64];
	char *page = 0;
	unsigned *common, *stubs;
	int ns = 0, i;

	for (unsigned *q = (unsigned *) lo + 1; q < (unsigned *) hi && ns < 64; q++) {
		if (*q != 0xd4000001u)
			continue;
		for (int k = 1; k <= 8 && q - k >= (unsigned *) lo; k++) {   /* the mov is not always adjacent */
			unsigned w = q[-k];
			int x8 = (w & 0xffe0001fu) == 0xd2800008u;   /* movz x8,#imm */
			int w8 = (w & 0xffe0001fu) == 0x52800008u;   /* movz w8,#imm (clang's freestanding form) */

			if ((x8 || w8) && hook_path_reg((w >> 5) & 0xffff) >= 0) { sites[ns++] = q; break; }
		}
	}
	if (ns == 0)
		return;

	for (int k = 1; k <= 8 && page == 0; k++) {                    /* a stub page within B reach */
		char *hint = (char *) (((unsigned long) hi + ((unsigned long) k << 21)) & ~0xfffffUL);
		void *m = (void *) sc6(SYS_mmap, (long) hint, 4096 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m != (void *) -1 && (unsigned long) m > lo && (unsigned long) m - lo < (1UL << 27)) page = m;
		else if (m != (void *) -1) sc6(SYS_munmap, (long) m, 4096 * 4, 0, 0, 0, 0);
	}
	if (page == 0)
		return;

	emit_p = (unsigned *) page;
	common = emit_p;
	emit_common();
	stubs = emit_p;
	for (i = 0; i < ns; i++) {
		emit(sub_sp(SITE));
		emit(stp(29, 30, 232));
		emit(branch(0x94000000u, emit_p, common));              /* bl common */
		emit(ldp(29, 30, 232));
		emit(add_sp(SITE));
		emit(branch(0x14000000u, emit_p, (char *) sites[i] + 4));   /* b site+4 */
	}
	flush(page, emit_p);
	if (sc6(SYS_mprotect, (long) page, 4096 * 4, PROT_READ | PROT_EXEC, 0, 0, 0) != 0)
		return;

	uint64_t tlo = lo & ~4095UL, thi = (hi + 4095) & ~4095UL;        /* anonymous: write, then RX */
	if (sc6(SYS_mprotect, (long) tlo, thi - tlo, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) != 0)
		return;
	for (i = 0; i < ns; i++)
		*sites[i] = b_enc(sites[i], (char *) stubs + (unsigned long) i * 24);
	flush((void *) tlo, (void *) thi);
	sc6(SYS_mprotect, (long) tlo, thi - tlo, PROT_READ | PROT_EXEC, 0, 0, 0);
}
