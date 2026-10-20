/* ldhook: an LD_AUDIT module.  At la_version -- before the loader opens the shim,
 * libc or the program's libraries -- it patches the loader's own path syscalls
 * (`openat`, `faccessat`) to a stub that maps the path through the one law
 * (policy.c, built -DDN_FREE) and sets the lane cookie in x5, so the tracer's
 * filter lets the mapped call through with no stop.
 *
 * The loader is what the shim cannot reach: its opens happen before any preloaded
 * object's constructor runs (E4-E6), and they are the biggest block of stops a
 * build has (the loader's startup, one per process).  Its paths are the tree's own
 * now (the exec gate hands the program and --library-path as guest paths), so the
 * stub cannot just add the cookie: it must resolve each path to a host path, the
 * same law the tracer and the shim use.
 *
 * Freestanding: it links no libc (it would be pulled in before the loader means
 * to), so the law brings its own string helpers (DN_FREE) and readlink goes
 * through the lane (pol_readlink).  The prefix root is found the same way ld.so's
 * text is: from /proc/self/maps, by this module's own host path.
 *
 * See docs/reference/syscall-patching.md and experiments.md, E5-E7. */
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "policy.h"                     /* the law (u64/i64, policy_path_nf, ...) */

/* Freestanding: do not take <link.h>.  Its aarch64 audit prototypes name
 * La_aarch64_regs/La_aarch64_retval, so defining the same functions with the
 * opaque pointers this ABI-only module wants is a conflict (gcc 14, glibc
 * 2.41); the loader matches the symbols, not the C types.  aarch64 only. */
#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif
struct link_map;                /* la_objopen's parameter: opaque here */

#define COOKIE 0x9e1ab6c3d7f2a541UL
#define TAIL   "/usr/lib/deb-native/dn-ldhook.so"
#ifndef LAV_CURRENT
#define LAV_CURRENT 1          /* the audit interface version (glibc's link.h) */
#endif

/* the loader's path calls that stop, and are in the lane: openat, faccessat */
static int wanted(unsigned n) { return n == 56 || n == 48; }

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc 0\n\tmov x5, xzr" : "+r"(x0), "+r"(x5) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}
/* the module's own openat, with the lane cookie, so it does not itself stop */
static long openat_c(const char *p) {
    register long x8 __asm__("x8") = 56;
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long) p;
    register long x2 __asm__("x2") = 0;
    register long x5 __asm__("x5") = (long) COOKIE;
    __asm__ volatile("svc 0\n\tmov x5, xzr" : "+r"(x0), "+r"(x5) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

/* the law's one syscall (policy.h): readlink of a host path, on the lane, so the
 * walk costs no stop */
ssize_t pol_readlink(const char *host, char *buf, size_t n) {
    register long x8 __asm__("x8") = 78;
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long) host;
    register long x2 __asm__("x2") = (long) buf;
    register long x3 __asm__("x3") = (long) n;
    register long x5 __asm__("x5") = (long) COOKIE;
    __asm__ volatile("svc 0\n\tmov x5, xzr" : "+r"(x0), "+r"(x5) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

/* the law's second syscall: the owner store (unused by the loader hook, but
 * policy.c links it) */
ssize_t pol_getxattr(const char *host, int nofollow, const char *name, char *buf, size_t n) {
    register long x8 __asm__("x8") = nofollow ? 9 : 8;
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long) host;
    register long x2 __asm__("x2") = (long) name;
    register long x3 __asm__("x3") = (long) buf;
    register long x4 __asm__("x4") = (long) n;
    register long x5 __asm__("x5") = (long) COOKIE;
    __asm__ volatile("svc 0\n\tmov x5, xzr" : "+r"(x0), "+r"(x5) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static int has(const char *hay, long hn, const char *pat) {
    long pn = 0, i, j; while (pat[pn]) pn++;
    for (i = 0; i + pn <= hn; i++) { for (j = 0; j < pn && hay[i + j] == pat[j]; j++) { } if (j == pn) return 1; }
    return 0;
}

/* read /proc/self/maps once, into b; the length, or -1 */
static long read_maps(char *b, long cap) {
    long fd = openat_c("/proc/self/maps"), n, tot = 0;
    if (fd < 0) return -1;
    while (tot < cap - 1 && (n = sc6(63, fd, (long) (b + tot), cap - 1 - tot, 0, 0, 0)) > 0) tot += n;
    sc6(57, fd, 0, 0, 0, 0, 0);
    b[tot] = 0;
    return tot;
}

/* find ld.so's r-xp segment, and this module's own host path, in maps */
static int ld_text(const char *b, long tot, unsigned long *lo, unsigned long *hi) {
    for (long i = 0; i < tot; ) {
        long j = i; while (j < tot && b[j] != '\n') j++;
        long L = j - i;
        if (has(b + i, L, "ld-linux") && has(b + i, L, "r-xp")) {
            unsigned long a = 0, c = 0; long k = i;
            while (k < j && b[k] != '-') { a = a * 16 + (b[k] <= '9' ? b[k] - '0' : (b[k] | 32) - 'a' + 10); k++; }
            k++; while (k < j && b[k] != ' ') { c = c * 16 + (b[k] <= '9' ? b[k] - '0' : (b[k] | 32) - 'a' + 10); k++; }
            *lo = a; *hi = c; return 1;
        }
        i = j + 1;
    }
    return 0;
}
/* AT_BASE from /proc/self/auxv: the loader's own base.  dn-exec maps the
 * loader from anonymous memory, so its maps line has no "ld-linux" path. */
static unsigned long auxv_base(void) {
    char b[128];
    long fd = openat_c("/proc/self/auxv"), n;
    if (fd < 0) return 0;
    n = sc6(63, fd, (long) b, sizeof b, 0, 0, 0);
    sc6(57, fd, 0, 0, 0, 0, 0);
    if (n < 16) return 0;
    for (long i = 0; i + 15 < n; i += 16) {
        unsigned long k = 0, v = 0;
        for (int j = 0; j < 8; j++) k |= (unsigned long) (unsigned char) b[i + j] << (j * 8);
        for (int j = 0; j < 8; j++) v |= (unsigned long) (unsigned char) b[i + 8 + j] << (j * 8);
        if (k == 0) break;
        if (k == 7) return v;
    }
    return 0;
}

/* The loader's entry segment, found by AT_BASE when the maps line has no path. */
static int anon_text(const char *b, long tot, unsigned long base,
                     unsigned long *lo, unsigned long *hi) {
    unsigned long best = ~0UL, blo = 0, bhi = 0;
    if (!base) return 0;
    for (long i = 0; i < tot; ) {
        long j = i; while (j < tot && b[j] != '\n') j++;
        long L = j - i;
        if (has(b + i, L, "r-xp")) {
            unsigned long a = 0, c = 0; long k = i;
            while (k < j && b[k] != '-') { a = a * 16 + (b[k] <= '9' ? b[k] - '0' : (b[k] | 32) - 'a' + 10); k++; }
            k++; while (k < j && b[k] != ' ') { c = c * 16 + (b[k] <= '9' ? b[k] - '0' : (b[k] | 32) - 'a' + 10); k++; }
            if (a >= base && a - base < 0x400000UL && a < best) { best = a; blo = a; bhi = c; }
        }
        i = j + 1;
    }
    if (best == ~0UL) return 0;
    *lo = blo; *hi = bhi; return 1;
}

/* the module's own host path, into out; 1 if found */
static int self_path(const char *b, long tot, char *out, long cap) {
    for (long i = 0; i < tot; ) {
        long j = i; while (j < tot && b[j] != '\n') j++;
        if (has(b + i, j - i, TAIL)) {
            long k = i; long sp = -1;                 /* the path is the last field */
            for (; k < j; k++) if (b[k] == ' ') sp = k;
            if (sp < 0 || sp + 1 >= j) { i = j + 1; continue; }
            long n = j - (sp + 1);
            if (n >= cap) n = cap - 1;
            for (long m = 0; m < n; m++) out[m] = b[sp + 1 + m];
            out[n] = 0;
            return 1;
        }
        i = j + 1;
    }
    return 0;
}

/* the stub's frame: x0..x28 (0..231), nzcv (248), x30 (256), then room for the
 * mapped path (272..).  The hook runs on the calling thread's stack (saved below
 * the frame), and writes the host path into that room. */
#define FRAME 272
#define PBUF  512
#define SITE  (FRAME + PBUF)            /* the per-site stub's frame (16-aligned) */

/* Map a call's path through the law and set the lane cookie, in the saved frame
 * r (r[0]=x0 ... r[8]=x8).  Only the loader's path calls are patched, so r[8] is
 * openat or faccessat, both with the path in x1. */
static void dh_hook(unsigned long *r) {
    long nr = (long) r[8];

    if (wanted((unsigned) nr)) {
        const char *path = (const char *) r[1];

        if (path && path[0] == '/') {
            char *host = (char *) r + FRAME;
            int n = policy_path_nf(path, host, PBUF, 0);

            if (n > 0)
                r[1] = (unsigned long) host;    /* a guest path: the tracer sees the host's */
        }
        r[5] = (unsigned long) COOKIE;          /* the filter lets the mapped call through */
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

static void flush(void *lo, void *hi) {
    __asm__ volatile("dsb ish" ::: "memory");
    for (char *p = (char *) ((unsigned long) lo & ~63UL); p < (char *) hi; p += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (char *p = (char *) ((unsigned long) lo & ~63UL); p < (char *) hi; p += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* The shared block: save x0..x28, nzcv and x30; call the hook with x0 = the
 * frame; restore; make the call.  The per-site stubs branch here. */
static unsigned *emit_p;
static void emit(unsigned i) { *emit_p++ = i; }
static void emit_common(void) {
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
    emit(0);                                    /* ldr x16, =dh_hook (set below) */
    emit(0xd63f0200u);                          /* blr x16 */
    emit(ldrx(9, 248));
    emit(0xd51b4209u);                          /* msr nzcv, x9 */
    emit(ldrx(30, 256));
    for (i = 0; i < 28; i += 2)
        emit(ldp(i, i + 1, i * 8));
    emit(ldrx(28, 224));
    emit(0xd4000001u);                          /* svc #0 */
    emit(0xaa1f03e5u);                          /* mov x5, xzr: the cookie is for this svc only */
    emit(0xd65f03c0u);                          /* ret */
    if ((unsigned long) emit_p & 4)
        emit(0xd503201fu);                      /* nop: the literal is 8-aligned */
    *lit = 0x58000000u | ((unsigned) (emit_p - lit) & 0x7ffff) << 5 | 16u;
    *(void **) emit_p = (void *) dh_hook;
    emit_p += 2;
}

__attribute__((visibility("default"))) unsigned int la_version(unsigned int v) {
    static char maps[32768], root[512];
    unsigned long lo = 0, hi = 0;
    long tot = read_maps(maps, sizeof maps);
    char *page = MAP_FAILED;
    unsigned *common, *stubs, *sites[64];
    int ns = 0, i;

    if (tot <= 0 || !self_path(maps, tot, root, sizeof root))
        return v ? v : LAV_CURRENT;
    if (!ld_text(maps, tot, &lo, &hi) && !anon_text(maps, tot, auxv_base(), &lo, &hi))
        return v ? v : LAV_CURRENT;
    {   /* the root is this module's host path minus the tail */
        size_t n = 0, t = 0; while (TAIL[t]) t++;
        while (root[n]) n++;
        if (n <= t || n - t >= sizeof root) return v ? v : LAV_CURRENT;
        root[n - t] = 0;
    }
    policy_root_set(root);
    {   /* the binds the host wrote before boot, so the loader maps through the table */
        char bp[600], b[4096];
        size_t ri = 0, rj;
        const char *s = "/boot/binds";
        long fd, k;

        while (root[ri] && ri + 12 < sizeof bp) { bp[ri] = root[ri]; ri++; }
        for (rj = 0; s[rj]; rj++) bp[ri + rj] = s[rj];
        bp[ri + rj] = '\0';
        fd = openat_c(bp);
        if (fd >= 0) {
            k = sc6(63, fd, (long) b, sizeof b - 1, 0, 0, 0);
            sc6(57, fd, 0, 0, 0, 0, 0);
            if (k > 0) { b[k] = '\0'; policy_binds_parse(b); }
        }
    }

    for (unsigned *q = (unsigned *) lo + 1; q < (unsigned *) hi && ns < 64; q++) {
        if (*q != 0xd4000001u) continue;
        for (int k = 1; k <= 8 && q - k >= (unsigned *) lo; k++)     /* the mov is not always adjacent */
            if ((q[-k] & 0xffe0001fu) == 0xd2800008u && wanted((q[-k] >> 5) & 0xffff)) { sites[ns++] = q; break; }
    }
    if (ns == 0) return v ? v : LAV_CURRENT;
    /* a stub page within a B's reach of the text */
    for (int k = 1; k <= 8 && page == MAP_FAILED; k++) {
        char *hint = (char *) (((unsigned long) hi + ((unsigned long) k << 21)) & ~0xfffffUL);
        void *m = (void *) sc6(222, (long) hint, 4096 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED && (unsigned long) m > lo && (unsigned long) m - lo < (1UL << 27)) page = m;
        else if (m != MAP_FAILED) sc6(215, (long) m, 4096 * 4, 0, 0, 0, 0);
    }
    if (page == MAP_FAILED) return v ? v : LAV_CURRENT;

    emit_p = common = (unsigned *) page;
    emit_common();
    stubs = emit_p;
    for (i = 0; i < ns; i++) {
        emit(sub_sp(SITE));
        emit(stp(29, 30, 232));
        emit(branch(0x94000000u, emit_p, common));      /* bl common */
        emit(ldp(29, 30, 232));
        emit(add_sp(SITE));
        emit(branch(0x14000000u, emit_p, (char *) sites[i] + 4));   /* b site+4 */
    }
    flush(page, emit_p);
    if (sc6(226, (long) page, 4096 * 4, PROT_READ | PROT_EXEC, 0, 0, 0) != 0)   /* execmem */
        return v ? v : LAV_CURRENT;
    /* patch the loader's text (execmod), then restore RX */
    unsigned long tlo = lo & ~4095UL, thi = (hi + 4095) & ~4095UL;
    if (sc6(226, (long) tlo, thi - tlo, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) != 0)
        return v ? v : LAV_CURRENT;
    for (i = 0; i < ns; i++)
        *sites[i] = b_enc(sites[i], (char *) stubs + (unsigned long) i * 24);
    flush((void *) tlo, (void *) thi);
    sc6(226, (long) tlo, thi - tlo, PROT_READ | PROT_EXEC, 0, 0, 0);
    return v ? v : LAV_CURRENT;
}
__attribute__((visibility("default"))) void la_preinit(uintptr_t *c) { (void) c; }
__attribute__((visibility("default"))) unsigned int la_objopen(struct link_map *m, long l, uintptr_t *c) { (void) m; (void) l; (void) c; return 0; }
/* glibc's audit interface requires the whole symbol set (version 2 too); the
 * module binds nothing (la_objopen returns 0), so these are stubs. */
__attribute__((visibility("default"))) char *la_objsearch(const char *name, uintptr_t *c, unsigned int flag)
{
	(void) c;
	(void) flag;
	return (char *) name;
}
__attribute__((visibility("default"))) void la_activity(uintptr_t *c, unsigned int flag)
{
	(void) c;
	(void) flag;
}
__attribute__((visibility("default"))) unsigned int la_objclose(uintptr_t *c)
{
	(void) c;
	return 0;
}
__attribute__((visibility("default"))) uintptr_t la_symbind64(ElfW(Sym) *sym, unsigned int ndx,
		uintptr_t *refcook, uintptr_t *defcook, unsigned int *flags, const char *name)
{
	(void) ndx; (void) refcook; (void) defcook; (void) flags; (void) name;
	return (uintptr_t) sym->st_value;
}
__attribute__((visibility("default"))) ElfW(Addr) la_aarch64_gnu_pltenter(ElfW(Sym) *sym,
		unsigned int ndx, uintptr_t *refcook, uintptr_t *defcook, void *regs,
		unsigned int *flags, const char *name, long *framesizep)
{
	(void) sym; (void) ndx; (void) refcook; (void) defcook; (void) regs;
	(void) flags; (void) name;
	if (framesizep)
		*framesizep = 0;
	return 0;
}
__attribute__((visibility("default"))) unsigned int la_aarch64_gnu_pltexit(ElfW(Sym) *sym,
		unsigned int ndx, uintptr_t *refcook, uintptr_t *defcook, const void *inregs,
		void *outregs, const char *name)
{
	(void) sym; (void) ndx; (void) refcook; (void) defcook; (void) inregs;
	(void) outregs; (void) name;
	return 0;
}
