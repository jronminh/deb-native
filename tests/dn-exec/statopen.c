/* A freestanding static program that opens guest paths, for the dn-exec
 * static-patch POC: its openat sites are patched at birth, so the paths are
 * mapped through the law in-process (no tracer stop). */
typedef unsigned long u64;
typedef long i64;

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc6(64, 2, (i64) s, (i64) slen(s), 0, 0, 0); }
__attribute__((noreturn)) static void quit(i64 c) { sc6(93, c, 0, 0, 0, 0, 0); for (;;) {} }

void _start(void) {
    i64 fd = sc6(56, -100, (i64) "/etc/passwd", 0, 0, 0, 0);
    if (fd < 0) { put("open /etc/passwd failed\n"); quit(1); }
    sc6(57, fd, 0, 0, 0, 0, 0);
    fd = sc6(56, -100, (i64) "/etc/ld.so.cache", 0, 0, 0, 0);
    if (fd < 0) { put("open /etc/ld.so.cache failed\n"); quit(1); }
    sc6(57, fd, 0, 0, 0, 0, 0);
    {
        long sb[16];

        if (sc6(43, (i64) "/usr", (i64) sb, 0, 0, 0, 0) != 0) { put("statfs /usr failed\n"); quit(1); }
    }
    put("static-ok\n");
    quit(0);
}
