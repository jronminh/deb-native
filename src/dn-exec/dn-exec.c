/*
 * dn-exec -- a userland exec for the prefix.
 *
 * The POC (docs/spec/loader-poc.md): map the program's PT_LOAD segments and,
 * for a dynamic program, the tree's PT_INTERP loader, build the stack and
 * auxv the kernel would build, and jump.  Freestanding, no libc.
 *
 * Usage:
 *   dn-exec [--root DIR] PROGRAM [ARG...]
 *
 * PROGRAM is a guest path when --root is given, a host path otherwise.
 * Dynamic programs use the PT_INTERP resolved under --root; for the POC the
 * loader's libraries must be reachable from its own view (run under dn-tiny
 * and the law maps them, or set --root and the test uses host paths).
 */
#include "../dn-tiny/policy.h"     /* the law (u64/i64, pol_readlink), -DDN_FREE */

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_openat 56
#define SYS_close 57
#define SYS_pread64 67
#define SYS_write 64
#define SYS_exit 93
#define SYS_mmap 222
#define SYS_mprotect 226
#define SYS_read 63
#define SYS_readlinkat 78
#define SYS_munmap 215

#define COOKIE 0x9e1ab6c3d7f2a541UL

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10  /* POC: MAP_FIXED (the 4.14 kernel has no NOREPLACE) */

#define PAGE 4096UL
#define STACK_SIZE (1024UL * 1024UL)
#define MAX_VEC 256

#define AT_NULL 0
#define AT_PAGESZ 6
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_EXECFN 31
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_SYSINFO_EHDR 33

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_GNU_STACK 0x6474e551UL

typedef struct {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Phdr;

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

static i64 sc6c(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, u64 cookie)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    register i64 x6 __asm__("x6") = (i64) cookie;
    __asm__ volatile("mov x5, x6\n\tsvc 0\n\tmov x5, xzr" : "+r"(x0), "+r"(x5) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x6) : "memory");
    return x0;
}

/* the law's one syscall (policy.h): readlink of a host path, on the lane, so the
 * walk the loader's hook makes costs no stop. */
ssize_t pol_readlink(const char *host, char *buf, size_t n)
{
	return sc6c(SYS_readlinkat, AT_FDCWD, (i64) host, (i64) buf, (i64) n, 0, 0, COOKIE);
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static int strncmp2(const char *a, const char *b, u64 n)
{
    while (n--) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}
static void *memcpy2(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
static void *memset2(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memset(void *d, int c, u64 n) { return memset2(d, c, n); }
void *memcpy(void *d, const void *s, u64 n) { return memcpy2(d, s, n); }

static void put(const char *s) { sc6(SYS_write, 2, (i64)s, (i64)slen(s), 0, 0, 0); }
static void die(const char *s) { put("dn-exec: "); put(s); put("\n"); sc6(SYS_exit, 127, 0, 0, 0, 0, 0); for (;;) {} }

static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
static u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }

static int open_ro(const char *path, int host)
{
    i64 fd = host ? sc6c(SYS_openat, AT_FDCWD, (i64)path, O_RDONLY, 0, 0, 0, COOKIE)
                  : sc6(SYS_openat, AT_FDCWD, (i64)path, O_RDONLY, 0, 0, 0);
    return fd < 0 ? -1 : (int)fd;
}

/* read the ELF header and phdrs; returns 0 on success, -1 on a bad image */
static int read_elf(int fd, Ehdr *eh, Phdr *ph, char *interp, u64 icap) {
    if (sc6(SYS_pread64, fd, (i64)eh, sizeof *eh, 0, 0, 0) != (i64)sizeof *eh) return -1;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != 2 || eh->e_machine != 183) return -1;   /* ELF64, AArch64 */
    if (eh->e_phentsize != sizeof(Phdr) || eh->e_phnum == 0 || eh->e_phnum > 64) return -1;
    if (sc6(SYS_pread64, fd, (i64)ph, (i64)sizeof(Phdr) * eh->e_phnum, eh->e_phoff, 0, 0) !=
        (i64)sizeof(Phdr) * eh->e_phnum) return -1;
    if (interp) interp[0] = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP && interp) {
            u64 n = ph[i].p_filesz;
            if (n == 0 || n > icap || sc6(SYS_pread64, fd, (i64)interp, (i64)n, ph[i].p_offset, 0, 0) != (i64)n)
                return -1;
            interp[n < icap ? n : icap - 1] = 0;
        }
    }
    return 0;
}

/* map one image (program or loader); returns the load base, 0 on failure */
static u64 map_image(int fd, const Ehdr *eh, const Phdr *ph, u64 hint) {
    u64 lo = ~0UL, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (lo == ~0UL || hi <= lo) return ~0UL;

    u64 base = 0, load = 0;
    if (eh->e_type == 2) {                       /* ET_EXEC: fixed */
        base = 0;
    } else {                                     /* ET_DYN: pick a base */
        base = hint ? align_down(hint, 0x10000UL) : 0x40000000UL;
        for (int i = 0; i < 16; i++) {
            load = base;
            u64 addr = load + align_down(lo, PAGE);
            i64 r = sc6(SYS_mmap, (i64)addr, (i64)(align_up(hi, PAGE) - align_down(lo, PAGE)),
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if ((u64)r == addr) goto mapped;
            base += 0x10000000UL;
        }
        return ~0UL;
    }
    {
        u64 addr = load + align_down(lo, PAGE);
        u64 len = align_up(hi, PAGE) - align_down(lo, PAGE);
        i64 r = sc6(SYS_mmap, (i64)addr, (i64)len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if ((u64)r != addr) return ~0UL;
    }

mapped:
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        u64 dst = load + ph[i].p_vaddr;
        if (ph[i].p_filesz) {
            if (sc6(SYS_pread64, fd, (i64)dst, (i64)ph[i].p_filesz, ph[i].p_offset, 0, 0) != (i64)ph[i].p_filesz)
                return ~0UL;
        }
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset2((void *)(dst + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
        int prot = 0;
        if (ph[i].p_flags & 4) prot |= PROT_READ;
        if (ph[i].p_flags & 2) prot |= PROT_WRITE;
        if (ph[i].p_flags & 1) prot |= PROT_EXEC;
        u64 ps = align_down(dst, PAGE);
        u64 pe = align_up(dst + ph[i].p_memsz, PAGE);
        if (sc6(SYS_mprotect, (i64)ps, (i64)(pe - ps), prot, 0, 0, 0) != 0) return ~0UL;
    }
    return load;
}

/* the executable range [*lo, *hi) of the image mapped at `base`; 1 if any */
static int exec_range(u64 base, const Ehdr *eh, const Phdr *ph, u64 *lo, u64 *hi)
{
	u64 l = ~0UL, h = 0;

	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & 1)) continue;
		u64 s = base + align_down(ph[i].p_vaddr, PAGE);
		u64 e = base + align_up(ph[i].p_vaddr + ph[i].p_memsz, PAGE);
		if (s < l) l = s;
		if (e > h) h = e;
	}
	if (l == ~0UL) return 0;
	*lo = l; *hi = h;
	return 1;
}

/* Set the law's root and read the tree's binds, so the loader's opens map
 * through the same table the tracer uses.  Only with --root: without it the
 * loader's opens go to the tracer untouched. */
static void law_init(const char *root)
{
	char bp[1024 + 16], b[4096];
	u64 rl = slen(root);

	policy_root_set(root);
	if (rl + 12 < sizeof bp) {
		memcpy2(bp, root, rl);
		memcpy2(bp + rl, "/boot/binds", 12);
		long fd = sc6(SYS_openat, AT_FDCWD, (i64) bp, O_RDONLY, 0, 0, 0);
		if (fd >= 0) {
			long k = sc6(SYS_read, fd, (i64) b, sizeof b - 1, 0, 0, 0);
			sc6(SYS_close, fd, 0, 0, 0, 0, 0);
			if (k > 0) {
				b[k] = '\0';
				policy_binds_parse(b);
			}
		}
	}
}

/* ---- patch the loader's own syscall sites (openat/faccessat) at birth ----
 * The loader is mapped from anonymous memory, so the patch is a write into
 * memory dn-exec owns -- no execmod, unlike the LD_AUDIT hook this replaces. */

#define FRAME 272
#define PBUF  512
#define SITE  (FRAME + PBUF)

static int wanted(unsigned n) { return n == 56 || n == 48; }   /* openat, faccessat */

/* The hook: map the path (in x1) through the one law and mark the call with the
 * lane cookie, so the filter lets the already-mapped call through without a stop. */
static void exec_hook(unsigned long *r)
{
	long nr = (long) r[8];

	if (wanted((unsigned) nr)) {
		const char *path = (const char *) r[1];

		if (path && path[0] == '/') {
			char *host = (char *) r + FRAME;
			int n = policy_path_nf(path, host, PBUF, 0);

			if (n > 0)
				r[1] = (unsigned long) host;
		}
		r[5] = (unsigned long) COOKIE;
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

/* Patch the openat/faccessat `mov x8,#nr; svc #0` sites in [lo, hi).  The same
 * function maps a static program later: it is the one fast-path mechanism for
 * whatever dn-exec maps. */
static void patch_image(u64 lo, u64 hi)
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

			if ((x8 || w8) && wanted((w >> 5) & 0xffff)) { sites[ns++] = q; break; }
		}
	}
	if (ns == 0)
		return;

	for (int k = 1; k <= 8 && page == 0; k++) {                    /* a stub page within B reach */
		char *hint = (char *) (((unsigned long) hi + ((unsigned long) k << 21)) & ~0xfffffUL);
		void *m = (void *) sc6(SYS_mmap, (i64) hint, 4096 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m != (void *) -1 && (unsigned long) m > lo && (unsigned long) m - lo < (1UL << 27)) page = m;
		else if (m != (void *) -1) sc6(SYS_munmap, (i64) m, 4096 * 4, 0, 0, 0, 0);
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
	if (sc6(SYS_mprotect, (i64) page, 4096 * 4, PROT_READ | PROT_EXEC, 0, 0, 0) != 0)
		return;

	u64 tlo = lo & ~4095UL, thi = (hi + 4095) & ~4095UL;        /* anonymous: write, then RX */
	if (sc6(SYS_mprotect, (i64) tlo, thi - tlo, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) != 0)
		return;
	for (i = 0; i < ns; i++)
		*sites[i] = b_enc(sites[i], (char *) stubs + (unsigned long) i * 24);
	flush((void *) tlo, (void *) thi);
	sc6(SYS_mprotect, (i64) tlo, thi - tlo, PROT_READ | PROT_EXEC, 0, 0, 0);
}

static char *push_str(char **sp, const char *s) {
    u64 n = slen(s) + 1;
    *sp -= n;
    memcpy2(*sp, s, n);
    return *sp;
}

static void push_word(char **sp, u64 v) {
    *sp -= 8;
    *(u64 *)*sp = v;
}

/* build the initial stack; returns the final sp (16-aligned) */
static u64 build_stack(char *top, int argc, char **argv, char **envp, u64 phdr, u64 phent, u64 phnum,
                       u64 base, u64 entry, const char *execfn, u64 hwcap, u64 hwcap2, u64 sysinfo) {
    char *sp = top;
    char *argp[MAX_VEC], *envpp[MAX_VEC];
    int envc = 0;

    if (argc > MAX_VEC) argc = MAX_VEC;
    for (int i = 0; i < argc; i++) argp[i] = push_str(&sp, argv[i]);
    for (envc = 0; envp && envp[envc] && envc < MAX_VEC - 1; envc++) envpp[envc] = push_str(&sp, envp[envc]);
    char *execp = execfn ? push_str(&sp, execfn) : 0;
    char *platform = push_str(&sp, "aarch64");
    static const char rndbytes[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    sp -= 16; memcpy2(sp, rndbytes, 16); char *rnd = sp;

    sp = (char *)align_down((u64)sp, 16);

    struct { u64 k, v; } av[24];
    int n = 0;
    av[n].k = AT_PAGESZ; av[n++].v = PAGE;
    av[n].k = AT_PHDR;   av[n++].v = phdr;
    av[n].k = AT_PHENT;  av[n++].v = phent;
    av[n].k = AT_PHNUM;  av[n++].v = phnum;
    av[n].k = AT_BASE;   av[n++].v = base;
    av[n].k = AT_ENTRY;  av[n++].v = entry;
    av[n].k = AT_EXECFN; av[n++].v = (u64)execp;
    av[n].k = AT_PLATFORM; av[n++].v = (u64)platform;
    av[n].k = AT_RANDOM; av[n++].v = (u64)rnd;
    av[n].k = AT_HWCAP; av[n++].v = hwcap;
    av[n].k = AT_HWCAP2; av[n++].v = hwcap2;
    av[n].k = AT_SYSINFO_EHDR; av[n++].v = sysinfo;
    av[n].k = AT_CLKTCK; av[n++].v = 100;
    av[n].k = AT_SECURE; av[n++].v = 0;
    av[n].k = AT_NULL;   av[n++].v = 0;

    u64 words = (u64)n * 2 + 1 + (u64)envc + 1 + (u64)argc + 1;
    if (words & 1) push_word(&sp, 0);             /* padding above the arrays */

    for (int i = n - 1; i >= 0; i--) { push_word(&sp, av[i].v); push_word(&sp, av[i].k); }
    push_word(&sp, 0);                            /* envp terminator */
    for (int i = envc - 1; i >= 0; i--) push_word(&sp, (u64)envpp[i]);
    push_word(&sp, 0);                            /* argv terminator */
    for (int i = argc - 1; i >= 0; i--) push_word(&sp, (u64)argp[i]);
    push_word(&sp, (u64)argc);
    return (u64)sp;
}

__attribute__((naked, noreturn)) static void jump_to(u64 sp, u64 entry) {
    __asm__ volatile("mov sp, x0\n\tbr x1");
}

__attribute__((used)) void dn_main(u64 *init_sp) {
    int argc = (int)init_sp[0];
    char **argv = (char **)&init_sp[1];
    char **envp = argv + argc + 1;
    int envc0 = 0; while (envp[envc0]) envc0++;
    u64 *av0 = (u64 *)(envp + envc0 + 1);
    u64 hwcap = 0, hwcap2 = 0, sysinfo = 0;
    for (u64 *a = av0; a[0]; a += 2) {
        if (a[0] == AT_HWCAP) hwcap = a[1];
        else if (a[0] == AT_HWCAP2) hwcap2 = a[1];
        else if (a[0] == AT_SYSINFO_EHDR) sysinfo = a[1];
    }

    char root[1024];
    root[0] = 0;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == 'r' && argv[i][3] == 'o' &&
        argv[i][4] == 'o' && argv[i][5] == 't' && argv[i][6] == 0) {
        if (i + 1 >= argc) die("--root needs a directory");
        u64 n = slen(argv[i + 1]);
        if (n >= sizeof root) die("root path too long");
        memcpy2(root, argv[i + 1], n + 1);
        i += 2;
    }
    if (i >= argc) die("no program");
    if (root[0]) law_init(root);
    const char *prog = argv[i];
    char path[4096];
    if (root[0]) {
        u64 rl = slen(root), pl = slen(prog);
        if (rl + pl + 1 >= sizeof path) die("path too long");
        memcpy2(path, root, rl);
        memcpy2(path + rl, prog, pl + 1);
    } else {
        u64 pl = slen(prog);
        if (pl >= sizeof path) die("path too long");
        memcpy2(path, prog, pl + 1);
    }

    int fd = open_ro(path, root[0] != 0);
    if (fd < 0) die("open program");
    Ehdr eh; Phdr ph[64]; char interp[4096];
    if (read_elf(fd, &eh, ph, interp, sizeof interp) < 0) die("bad ELF");

    u64 prog_base = map_image(fd, &eh, ph, 0x40000000UL);
    if (prog_base == ~0UL) die("map program");
    u64 prog_entry = prog_base + eh.e_entry;

    u64 load_base = 0, entry = prog_entry;
    if (!interp[0] && root[0]) {                    /* static: patch its own sites */
        u64 plo, phi;

        if (exec_range(prog_base, &eh, ph, &plo, &phi))
            patch_image(plo, phi);
    }
    if (interp[0]) {
        char ipath[4096];
        if (root[0]) {
            u64 rl = slen(root), il = slen(interp);
            if (rl + il + 1 >= sizeof ipath) die("interp path too long");
            memcpy2(ipath, root, rl); memcpy2(ipath + rl, interp, il + 1);
        } else {
            u64 il = slen(interp);
            if (il >= sizeof ipath) die("interp path too long");
            memcpy2(ipath, interp, il + 1);
        }
        int lfd = open_ro(ipath, root[0] != 0);
        if (lfd < 0) die("open loader");
        Ehdr leh; Phdr lph[64];
        if (read_elf(lfd, &leh, lph, 0, 0) < 0) die("bad loader");
        load_base = map_image(lfd, &leh, lph, 0x50000000UL);
        if (load_base == ~0UL) die("map loader");
        entry = load_base + leh.e_entry;
        if (root[0]) {
            u64 lexe_lo, lexe_hi;

            if (exec_range(load_base, &leh, lph, &lexe_lo, &lexe_hi))
                patch_image(lexe_lo, lexe_hi);
        }
        sc6(SYS_close, lfd, 0, 0, 0, 0, 0);
    }
    sc6(SYS_close, fd, 0, 0, 0, 0, 0);

    i64 st = sc6(SYS_mmap, 0, STACK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st < 0) die("map stack");

    /* The L3 bootstrap: guest paths, not Android paths.  The shim is opened by
     * the loader through the patched sites (the law + lane); the loader's own
     * sites are patched at birth above, so no LD_AUDIT module is needed. */
    char *env2[MAX_VEC];
    int ne = 0;
    for (int e = 0; envp[e] && ne < MAX_VEC - 3; e++) {
        if (strncmp2(envp[e], "LD_PRELOAD=", 11) == 0 || strncmp2(envp[e], "LD_AUDIT=", 9) == 0)
            continue;
        env2[ne++] = envp[e];
    }
    env2[ne++] = "LD_PRELOAD=/usr/lib/deb-native/dn-lane.so";
    env2[ne] = 0;

    u64 sp = build_stack((char *)((u64)st + STACK_SIZE), argc - i, argv + i, env2,
                         prog_base + eh.e_phoff, sizeof(Phdr), eh.e_phnum, load_base, prog_entry,
                         prog, hwcap, hwcap2, sysinfo);
    jump_to(sp, entry);
    die("entry returned");
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("mov x0, sp\n\tbl dn_main\n\tmov x0, #127\n\tmov x8, #93\n\tsvc #0");
}
