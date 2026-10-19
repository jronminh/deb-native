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
typedef unsigned long u64;
typedef long i64;
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

static int open_ro(const char *path) {
    i64 fd = sc6(SYS_openat, AT_FDCWD, (i64)path, O_RDONLY, 0, 0, 0);
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

    int fd = open_ro(path);
    if (fd < 0) die("open program");
    Ehdr eh; Phdr ph[64]; char interp[4096];
    if (read_elf(fd, &eh, ph, interp, sizeof interp) < 0) die("bad ELF");

    u64 prog_base = map_image(fd, &eh, ph, 0x40000000UL);
    if (prog_base == ~0UL) die("map program");
    u64 prog_entry = prog_base + eh.e_entry;

    u64 load_base = 0, entry = prog_entry;
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
        int lfd = open_ro(ipath);
        if (lfd < 0) die("open loader");
        Ehdr leh; Phdr lph[64];
        if (read_elf(lfd, &leh, lph, 0, 0) < 0) die("bad loader");
        load_base = map_image(lfd, &leh, lph, 0x50000000UL);
        if (load_base == ~0UL) die("map loader");
        entry = load_base + leh.e_entry;
        sc6(SYS_close, lfd, 0, 0, 0, 0, 0);
    }
    sc6(SYS_close, fd, 0, 0, 0, 0, 0);

    i64 st = sc6(SYS_mmap, 0, STACK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st < 0) die("map stack");

    /* The L3 bootstrap: guest paths, not Android paths.  The shim/ldhook are
     * opened by the loader through the law; ldhook's root comes from its own
     * mapped host path. */
    char *env2[MAX_VEC];
    int ne = 0;
    for (int e = 0; envp[e] && ne < MAX_VEC - 3; e++) {
        if (strncmp2(envp[e], "LD_PRELOAD=", 11) == 0 || strncmp2(envp[e], "LD_AUDIT=", 9) == 0)
            continue;
        env2[ne++] = envp[e];
    }
    env2[ne++] = "LD_PRELOAD=/usr/lib/deb-native/dn-lane.so";
    env2[ne++] = "LD_AUDIT=/usr/lib/deb-native/dn-ldhook.so";
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
