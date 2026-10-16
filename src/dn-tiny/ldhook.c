/* ldhook: an LD_AUDIT module.  At la_version -- before the loader opens libc, the
 * shim and the program's libraries -- it patches the loader's own syscall sites
 * (`openat`, `faccessat`) to a stub that sets the lane cookie in x5 and re-issues
 * the call, so the tracer's filter lets it through with no stop.
 *
 * The loader is what the shim cannot reach: its opens happen before any preloaded
 * object's constructor runs (E4-E6), and they are the biggest block of stops a
 * build has (the loader's startup, one per process).  The loader's paths are host
 * paths (the exec gate passes host paths), so the stub needs no path law, only the
 * cookie -- except its `faccessat` on `/etc/ld.so.preload`, a guest path; that one
 * is allowed by luck (neither the host's nor the prefix's file exists).
 *
 * Freestanding: it must not link libc, or it would pull it in before the loader
 * means to.  See docs/reference/syscall-patching.md and experiments.md, E5-E7. */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/mman.h>

#define COOKIE 0x9e1ab6c3d7f2a541UL
#ifndef LAV_CURRENT
#define LAV_CURRENT 1          /* the audit interface version (glibc's link.h) */
#endif

/* the loader's path calls that stop, and are in the lane: openat, faccessat */
static int wanted(unsigned n) { return n == 56 || n == 48; }

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
/* the module's own openat, with the lane cookie, so it does not itself stop */
static long openat_c(const char *p) {
    register long x8 __asm__("x8") = 56;
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long) p;
    register long x2 __asm__("x2") = 0;
    register long x5 __asm__("x5") = (long) COOKIE;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x5) : "memory");
    return x0;
}
static int has(const char *hay, long hn, const char *pat) {
    long pn = 0, i, j; while (pat[pn]) pn++;
    for (i = 0; i + pn <= hn; i++) { for (j = 0; j < pn && hay[i + j] == pat[j]; j++) { } if (j == pn) return 1; }
    return 0;
}
/* find ld.so's r-xp segment from /proc/self/maps */
static int ld_text(unsigned long *lo, unsigned long *hi) {
    static char b[32768];
    long fd = openat_c("/proc/self/maps"), n, tot = 0, i;
    if (fd < 0) return 0;
    while ((n = sc6(63, fd, (long) (b + tot), sizeof b - 1 - tot, 0, 0, 0)) > 0) tot += n;
    sc6(57, fd, 0, 0, 0, 0, 0);
    b[tot] = 0;
    for (i = 0; i < tot; ) {
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
static unsigned movz(int rd, unsigned i16) { return 0xd2800000u | (i16 << 5) | (unsigned) rd; }
static unsigned movk(int rd, unsigned i16, int hw) { return 0xf2800000u | ((unsigned) hw << 21) | (i16 << 5) | (unsigned) rd; }
static unsigned svc0(void) { return 0xd4000001u; }
static unsigned b_enc(void *from, void *to) { long d = ((char *) to - (char *) from) >> 2; return 0x14000000u | ((unsigned) d & 0x3ffffffu); }
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

__attribute__((visibility("default"))) unsigned int la_version(unsigned int v) {
    unsigned long lo = 0, hi = 0;
    if (!ld_text(&lo, &hi)) return v ? v : LAV_CURRENT;
    unsigned *p = (unsigned *) lo, *end = (unsigned *) hi;
    unsigned *sites[64]; int ns = 0;
    for (unsigned *q = p + 1; q < end && ns < 64; q++) {
        if (*q != 0xd4000001u) continue;
        for (int k = 1; k <= 8 && q - k >= p; k++)     /* the mov is not always adjacent */
            if ((q[-k] & 0xffe0001fu) == 0xd2800008u && wanted((q[-k] >> 5) & 0xffff)) { sites[ns++] = q; break; }
    }
    if (ns == 0) return v ? v : LAV_CURRENT;
    /* a stub page within a B's reach of the text */
    char *page = MAP_FAILED;
    for (int k = 1; k <= 8 && page == MAP_FAILED; k++) {
        char *hint = (char *) (((unsigned long) hi + ((unsigned long) k << 21)) & ~0xfffffUL);
        void *m = (void *) sc6(222, (long) hint, 4096 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED && (unsigned long) m > lo && (unsigned long) m - lo < (1UL << 27)) page = m;
        else if (m != MAP_FAILED) sc6(215, (long) m, 4096 * 4, 0, 0, 0, 0);
    }
    if (page == MAP_FAILED) return v ? v : LAV_CURRENT;
    unsigned *stub = (unsigned *) page;
    for (int i = 0; i < ns; i++) {
        unsigned *br;
        *stub++ = movz(5, 0xa541);
        *stub++ = movk(5, 0xd7f2, 1);
        *stub++ = movk(5, 0xb6c3, 2);
        *stub++ = movk(5, 0x9e1a, 3);
        *stub++ = svc0();
        br = stub; *stub++ = b_enc(br, (char *) sites[i] + 4);   /* back to the instruction after the svc */
    }
    flush(page, stub);
    sc6(226, (long) page, 4096 * 4, PROT_READ | PROT_EXEC, 0, 0, 0);   /* execmem */
    /* patch the loader's text (execmod), then restore RX */
    unsigned long tlo = lo & ~4095UL, thi = (hi + 4095) & ~4095UL;
    if (sc6(226, (long) tlo, thi - tlo, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) != 0)
        return v ? v : LAV_CURRENT;
    for (int i = 0; i < ns; i++)
        *sites[i] = b_enc(sites[i], (char *) page + (unsigned long) i * 24);
    flush((void *) tlo, (void *) thi);
    sc6(226, (long) tlo, thi - tlo, PROT_READ | PROT_EXEC, 0, 0, 0);
    return v ? v : LAV_CURRENT;
}
__attribute__((visibility("default"))) void la_preinit(uintptr_t *c) { (void) c; }
__attribute__((visibility("default"))) unsigned int la_objopen(struct link_map *m, long l, uintptr_t *c) { (void) m; (void) l; (void) c; return 0; }
