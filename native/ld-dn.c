/* ld-dn -- deb-native's program loader stub (docs/design-0.2.0.md).
 *
 * Every Debian program in the prefix names this file as its ELF interpreter
 * (PT_INTERP), so the kernel runs it first, however the program was started:
 * by name, by full path, from Termux's side, by another program. It then:
 *
 *   1. finds the prefix from the program's own PT_INTERP string
 *      ($DN/usr/lib/deb-native/ld-dn);
 *   2. builds a corrected environment: Termux's Bionic preload (termux-exec)
 *      moves to DN_BIONIC_PRELOAD -- a glibc process cannot load it -- and
 *      LD_PRELOAD becomes the path-redirect shim, DN_INSTDIR the prefix;
 *   3. maps glibc's real loader ($DN/usr/lib/ld-linux-aarch64.so.1, the
 *      libc6 stand-in's link into Termux's glibc) into this same process and
 *      jumps to it with the corrected stack, AT_BASE pointing at it.
 *
 * No re-exec: the program stays the process's executable (/proc/self/exe),
 * which restarting through glibc's loader would change. The approach of
 * NixOS's nix-ld.
 *
 * Freestanding: no libc, raw syscalls, static PIE with no relocations to
 * apply (the kernel maps it; nothing relocates it). Build:
 *   clang -O2 -static -nostdlib -ffreestanding -fno-builtin \
 *         -fno-stack-protector -fPIE -Wl,-pie -Wl,--no-dynamic-linker \
 *         -o ld-dn ld-dn.c
 */
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned short u16;

typedef struct {
  unsigned char e_ident[16];
  u16 e_type, e_machine;
  u32 e_version;
  u64 e_entry, e_phoff, e_shoff;
  u32 e_flags;
  u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;
typedef struct {
  u32 p_type, p_flags;
  u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7

#define SYS_openat 56
#define SYS_close 57
#define SYS_write 64
#define SYS_pread64 67
#define SYS_exit 93
#define SYS_munmap 215
#define SYS_mmap 222
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_CLOEXEC 02000000
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

/* Space _start reserves between this function's frame and the original
 * stack, for the rebuilt argc/argv/envp/auxv. */
#define RESERVE 0x10000

static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  register i64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b,
      x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
static int failed(i64 r) { return (u64)r > (u64)-4096; }

/* The compiler may still emit calls to these for copies and zeroing. */
void *memset(void *d, int c, u64 n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int has(const char *s, const char *p) {
  for (; *s; s++) if (starts(s, p)) return 1;
  return 0;
}
static void put(const char *s) { sys6(SYS_write, 2, (i64)s, (i64)slen(s), 0, 0, 0); }
static void die(const char *what, const char *arg) {
  put("ld-dn: "); put(what); if (arg) { put(": "); put(arg); } put("\n");
  sys6(SYS_exit, 127, 0, 0, 0, 0, 0);
  for (;;) {}
}
/* dst = a + b, NUL-terminated; dies if it does not fit. */
static char *cat2(char *dst, u64 cap, const char *a, const char *b) {
  u64 la = slen(a), lb = slen(b);
  if (la + lb + 1 > cap) die("path too long", a);
  memcpy(dst, a, la); memcpy(dst + la, b, lb); dst[la + lb] = 0;
  return dst;
}

static char g_dn[4096], g_ld[4096], g_env_pre[4200], g_env_inst[4200], g_env_bio[8300];
static Phdr g_ph[32];

struct ret { u64 sp, entry; };

struct ret ld_dn_main(u64 *sp) {
  long argc = (long)sp[0];
  char **argv = (char **)(sp + 1);
  char **envp = argv + argc + 1;
  long nenv = 0;
  while (envp[nenv]) nenv++;
  u64 *auxv = (u64 *)(envp + nenv + 1);
  long naux = 0;
  u64 phdr = 0, phnum = 0, pagesz = 4096;
  while (auxv[2 * naux] != AT_NULL) {
    if (auxv[2 * naux] == AT_PHDR) phdr = auxv[2 * naux + 1];
    if (auxv[2 * naux] == AT_PHNUM) phnum = auxv[2 * naux + 1];
    if (auxv[2 * naux] == AT_PAGESZ) pagesz = auxv[2 * naux + 1];
    naux++;
  }

  /* 1. The prefix, from the program's PT_INTERP. */
  const Phdr *pp = (const Phdr *)phdr;
  u64 bias = 0;
  const char *interp = 0;
  for (u64 i = 0; i < phnum; i++) if (pp[i].p_type == PT_PHDR) bias = phdr - pp[i].p_vaddr;
  for (u64 i = 0; i < phnum; i++) if (pp[i].p_type == PT_INTERP) interp = (const char *)(bias + pp[i].p_vaddr);
  if (!interp) die("no PT_INTERP in the program", 0);
  const char *suffix = "/usr/lib/deb-native/ld-dn";
  u64 li = slen(interp), ls = slen(suffix);
  if (li <= ls || li - ls >= sizeof g_dn || !starts(interp + li - ls, suffix)) die("not installed as $PREFIX/usr/lib/deb-native/ld-dn", interp);
  memcpy(g_dn, interp, li - ls); g_dn[li - ls] = 0;

  /* 2. The environment. */
  const char *inherited = 0;
  for (long i = 0; i < nenv; i++) if (starts(envp[i], "LD_PRELOAD=")) inherited = envp[i] + 11;
  int new_bio = inherited && *inherited && !has(inherited, "path-redirect.so");
  if (new_bio) cat2(g_env_bio, sizeof g_env_bio, "DN_BIONIC_PRELOAD=", inherited);
  cat2(g_env_pre, sizeof g_env_pre, "LD_PRELOAD=", g_dn);
  cat2(g_env_pre + slen(g_env_pre), sizeof g_env_pre - slen(g_env_pre), "", "/usr/lib/deb-native/path-redirect.so");
  cat2(g_env_inst, sizeof g_env_inst, "DN_INSTDIR=", g_dn);

  /* The new stack: argc, argv, envp (+3 of ours), auxv -- in the space
   * _start reserved just below the original stack. */
  u64 words = 1 + (u64)argc + 1 + (u64)nenv + 3 + 1 + 2 * ((u64)naux + 1);
  if (words * 8 + 64 > RESERVE) die("environment too large", 0);
  u64 *ns = (u64 *)(((u64)sp - words * 8) & ~(u64)15);
  u64 k = 0;
  ns[k++] = (u64)argc;
  for (long i = 0; i < argc; i++) ns[k++] = (u64)argv[i];
  ns[k++] = 0;
  for (long i = 0; i < nenv; i++) {
    if (starts(envp[i], "LD_PRELOAD=") || starts(envp[i], "DN_INSTDIR=")) continue;
    if (new_bio && starts(envp[i], "DN_BIONIC_PRELOAD=")) continue;
    ns[k++] = (u64)envp[i];
  }
  ns[k++] = (u64)g_env_pre;
  ns[k++] = (u64)g_env_inst;
  if (new_bio) ns[k++] = (u64)g_env_bio;
  ns[k++] = 0;
  u64 *nauxv = ns + k;

  /* 3. Map glibc's real loader. */
  cat2(g_ld, sizeof g_ld, g_dn, "/usr/lib/ld-linux-aarch64.so.1");
  i64 fd = sys6(SYS_openat, AT_FDCWD, (i64)g_ld, O_RDONLY | O_CLOEXEC, 0, 0, 0);
  if (failed(fd)) die("cannot open", g_ld);
  Ehdr eh;
  if (sys6(SYS_pread64, fd, (i64)&eh, sizeof eh, 0, 0, 0) != (i64)sizeof eh ||
      eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_machine != 183 ||
      eh.e_phnum > sizeof g_ph / sizeof g_ph[0])
    die("not an arm64 ELF loader", g_ld);
  if (sys6(SYS_pread64, fd, (i64)g_ph, eh.e_phnum * sizeof(Phdr), (i64)eh.e_phoff, 0, 0) != (i64)(eh.e_phnum * sizeof(Phdr)))
    die("cannot read program headers", g_ld);
  u64 pm = pagesz - 1, lo = ~(u64)0, hi = 0;
  for (int i = 0; i < eh.e_phnum; i++) {
    if (g_ph[i].p_type != PT_LOAD) continue;
    if ((g_ph[i].p_vaddr & ~pm) < lo) lo = g_ph[i].p_vaddr & ~pm;
    if (((g_ph[i].p_vaddr + g_ph[i].p_memsz + pm) & ~pm) > hi) hi = (g_ph[i].p_vaddr + g_ph[i].p_memsz + pm) & ~pm;
  }
  i64 area = sys6(SYS_mmap, 0, (i64)(hi - lo), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (failed(area)) die("cannot reserve memory for", g_ld);
  u64 base = (u64)area - lo;
  for (int i = 0; i < eh.e_phnum; i++) {
    Phdr *s = &g_ph[i];
    if (s->p_type != PT_LOAD) continue;
    int prot = (s->p_flags & 4 ? PROT_READ : 0) | (s->p_flags & 2 ? PROT_WRITE : 0) | (s->p_flags & 1 ? PROT_EXEC : 0);
    u64 start = s->p_vaddr & ~pm, fend = s->p_vaddr + s->p_filesz, fmapend = (fend + pm) & ~pm;
    u64 mend = (s->p_vaddr + s->p_memsz + pm) & ~pm;
    if (s->p_filesz) {
      i64 r = sys6(SYS_mmap, (i64)(base + start), (i64)(fmapend - start), prot, MAP_PRIVATE | MAP_FIXED, fd, (i64)(s->p_offset & ~pm));
      if (failed(r)) die("cannot map a segment of", g_ld);
    }
    if (s->p_memsz > s->p_filesz) {
      if ((prot & PROT_WRITE) && fend < fmapend && s->p_filesz) memset((void *)(base + fend), 0, fmapend - fend);
      u64 zstart = s->p_filesz ? fmapend : start;
      if (mend > zstart) {
        i64 r = sys6(SYS_mmap, (i64)(base + zstart), (i64)(mend - zstart), prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        if (failed(r)) die("cannot map the zeroed part of", g_ld);
      }
    }
  }
  sys6(SYS_close, fd, 0, 0, 0, 0, 0);

  /* auxv, with AT_BASE pointing at the real loader. */
  for (long i = 0; i <= naux; i++) {
    nauxv[2 * i] = auxv[2 * i];
    nauxv[2 * i + 1] = auxv[2 * i] == AT_BASE ? base : auxv[2 * i + 1];
  }
  struct ret r = { (u64)ns, base + eh.e_entry };
  return r;
}

/* Reserve RESERVE bytes below the kernel's stack for the rebuilt vector,
 * call ld_dn_main(original sp), then enter glibc's loader on the new stack
 * the way the kernel would have: sp at argc, x0 = 0. */
__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  mov x0, sp\n"
    "  sub sp, sp, #0x10000\n"
    "  bl ld_dn_main\n"
    "  mov sp, x0\n"
    "  mov x16, x1\n"
    "  mov x0, #0\n"
    "  br x16\n");
