/* ld-dn -- deb-native's program loader stub (docs/spec/design.md,
 * docs/spec/ld-dn-config.md).
 *
 * Every Debian program in the prefix names this file as its ELF interpreter
 * (PT_INTERP), so the kernel runs it first, however the program was started:
 * by name, by full path, from Termux's side, by another program. It then:
 *
 *   1. finds the prefix from the program's own PT_INTERP string
 *      ($DN/usr/lib/deb-native/ld-dn);
 *   2. builds a corrected environment from a compiled-in default policy,
 *      overlaid by $DN/etc/deb-native/ld-dn.conf (or $DN_CONFIG); the
 *      caller's LD_LIBRARY_PATH is merged in after the fixed dirs and
 *      DN_PRELOAD is appended to the preload list;
 *   3. maps glibc's real loader ($DN/usr/lib/ld-linux-aarch64.so.1, the
 *      libc6 stand-in's link into Termux's glibc) into this same process and
 *      jumps to it with the corrected stack, AT_BASE pointing at it.
 *
 * The policy (loader, library dirs, preloads, env set/unset, shim redirect
 * roots, per-program overrides) is data, so extending behaviour is a config
 * edit in the prefix, not a rebuild -- see docs/spec/ld-dn-config.md. The
 * compiled defaults reproduce the pre-config behaviour exactly, so a prefix
 * with no config file (the bootstrap case) is unchanged.
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
#define SYS_read 63
#define SYS_write 64
#define SYS_pread64 67
#define SYS_readlinkat 78
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

/* The version this loader reports under DN_REDIRECT_DEBUG. */
#define DN_VERSION "0.5.4"

/* Space _start reserves between this function's frame and the original
 * stack, for the rebuilt argc/argv/envp/auxv. */
#define RESERVE 0x10000

/* Policy table caps. Bounded on purpose: a freestanding loader has no
 * allocation, and a silently-unbounded config would be a footgun. Hitting a
 * cap warns and keeps what fit (fail-open), never truncates a live string. */
#define DN_PATH_MAX 4096
#define DN_CFG_MAX 16384
#define DN_ENV_MAX 2048
#define MAX_LIB 8
#define MAX_PRE 4
#define MAX_ENV 16
#define MAX_UNSET 16

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
static int same(const char *a, const char *b) { while (*a && *b) if (*a++ != *b++) return 0; return *a == *b; }
static int same_n(const char *a, const char *b, u64 n) {
  for (u64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static int ends_with(const char *s, const char *suffix) {
  u64 ls = slen(s), lf = slen(suffix);
  return ls >= lf && same_n(s + (ls - lf), suffix, lf);
}
static void put(const char *s) { sys6(SYS_write, 2, (i64)s, (i64)slen(s), 0, 0, 0); }
static void warn(const char *what, const char *arg) {
  put("ld-dn: "); put(what); if (arg) { put(": "); put(arg); } put("\n");
}
static void die(const char *what, const char *arg) {
  warn(what, arg);
  sys6(SYS_exit, 127, 0, 0, 0, 0, 0);
  for (;;) {}
}
/* dst = a + b, NUL-terminated; dies if it does not fit. For core strings
 * only -- the prefix and the fixed literals, which are known to fit. A
 * config-sourced value must go through join_prefix()/app(), which report
 * failure instead of killing every prefix process. */
static char *cat2(char *dst, u64 cap, const char *a, const char *b) {
  u64 la = slen(a), lb = slen(b);
  if (la + lb + 1 > cap) die("path too long", a);
  memcpy(dst, a, la); memcpy(dst + la, b, lb); dst[la + lb] = 0;
  return dst;
}
/* dst += s, NUL-terminated. Returns 0 on success, -1 if it would not fit;
 * the caller warns and skips, never dies. */
static int app(char *dst, u64 cap, const char *s) {
  u64 l = slen(dst), ls = slen(s);
  if (l + ls + 1 > cap) return -1;
  memcpy(dst + l, s, ls + 1);
  return 0;
}
static char g_dn[DN_PATH_MAX];

/* A "path inside the prefix": a leading / is the Debian path, anything else
 * is relative; both join under $DN. Returns 0, or -1 if it would not fit. */
static int join_prefix(char *dst, u64 cap, const char *val) {
  u64 a = slen(g_dn), v = slen(val);
  u64 need = (val[0] == '/') ? a + v + 1 : a + 1 + v + 1;
  if (need > cap) return -1;
  memcpy(dst, g_dn, a);
  if (val[0] == '/') { memcpy(dst + a, val, v + 1); }
  else { dst[a] = '/'; memcpy(dst + a + 1, val, v + 1); }
  return 0;
}
/* Copy src into dst with the token "$DN" replaced by the prefix. Returns 0,
 * or -1 if it would not fit (the caller skips the entry). */
static int expand(char *dst, u64 cap, const char *src) {
  u64 k = 0;
  while (*src) {
    if (src[0] == '$' && src[1] == 'D' && src[2] == 'N') {
      u64 l = slen(g_dn);
      if (k + l + 1 > cap) return -1;
      memcpy(dst + k, g_dn, l); k += l; src += 3;
    } else {
      if (k + 1 >= cap) return -1;
      dst[k++] = *src++;
    }
  }
  dst[k] = 0;
  return 0;
}
static const char *findenv(char **envp, long nenv, const char *name) {
  u64 n = slen(name);
  for (long i = 0; i < nenv; i++)
    if (same_n(envp[i], name, n) && envp[i][n] == '=') return envp[i] + n + 1;
  return 0;
}

/* ---- policy --------------------------------------------------------- */

static char cfg[DN_CFG_MAX];
static char g_loader[DN_PATH_MAX], g_default_pre[DN_PATH_MAX], g_rprefix[DN_PATH_MAX];
static char g_env_pre[8192], g_env_inst[4200], g_env_lib[8192], g_env_rprefix[4200], g_env_bio[8300];
static char lib_buf[MAX_LIB][DN_PATH_MAX], pre_buf[MAX_PRE][DN_PATH_MAX];
static char env_buf[MAX_ENV][DN_ENV_MAX], unset_buf[MAX_UNSET][64];
static const char *libs[MAX_LIB]; static int nlib;
static const char *pres[MAX_PRE]; static int npre;
static const char *sets[MAX_ENV]; static int nset;
static const char *unsets[MAX_UNSET]; static int nunset;
static int default_pre = 1, has_rprefix = 0, g_new_bio = 0;
static const char *g_inherited_lib, *g_extra_preload;
static char g_progexe[DN_PATH_MAX], g_progbase[256]; static int g_have_prog;

static Phdr g_ph[32];

static void add_lib(const char *val) {
  if (nlib >= MAX_LIB) { warn("config: too many lib-add, ignored", val); return; }
  if (join_prefix(lib_buf[nlib], DN_PATH_MAX, val) != 0) { warn("config: lib-add path too long, ignored", val); return; }
  libs[nlib] = lib_buf[nlib]; nlib++;
}
static void add_pre(const char *val) {
  if (npre >= MAX_PRE) { warn("config: too many preload, ignored", val); return; }
  if (join_prefix(pre_buf[npre], DN_PATH_MAX, val) != 0) { warn("config: preload path too long, ignored", val); return; }
  pres[npre] = pre_buf[npre]; npre++;
}
static void add_unset(const char *name) {
  if (!*name) return;
  for (int i = 0; i < nunset; i++) if (same(unsets[i], name)) return;
  if (nunset >= MAX_UNSET) { warn("config: too many unset, ignored", name); return; }
  u64 l = slen(name); if (l >= sizeof unset_buf[0]) l = sizeof unset_buf[0] - 1;
  memcpy(unset_buf[nunset], name, l); unset_buf[nunset][l] = 0;
  unsets[nunset] = unset_buf[nunset]; nunset++;
}
/* Set (or replace) an env entry. Builds into a local buffer first so a
 * too-long value leaves the table untouched. warn_skip distinguishes a
 * config error (warn) from a compiled default (silent -- known to fit). */
static int set_env(const char *k, const char *v, int warn_skip) {
  u64 kl = slen(k);
  if (!kl) return 0;
  if (kl + 1 >= DN_ENV_MAX) { if (warn_skip) warn("config: env key too long, ignored", k); return 0; }
  char tmp[DN_ENV_MAX];
  memcpy(tmp, k, kl); tmp[kl] = '=';
  if (expand(tmp + kl + 1, DN_ENV_MAX - kl - 1, v) != 0) {
    if (warn_skip) warn("config: env value too long, ignored", k);
    return 0;
  }
  int idx = -1;
  for (int i = 0; i < nset; i++) if (same_n(sets[i], k, kl) && sets[i][kl] == '=') { idx = i; break; }
  if (idx < 0) {
    if (nset >= MAX_ENV) { if (warn_skip) warn("config: too many env, ignored", k); return 0; }
    idx = nset++; sets[idx] = env_buf[idx];
  }
  memcpy(env_buf[idx], tmp, slen(tmp) + 1);
  return 1;
}
static int env_is_set(const char *e, u64 n) {
  for (int i = 0; i < nset; i++) if (same_n(sets[i], e, n) && sets[i][n] == '=') return 1;
  return 0;
}
static int skip_env(const char *e) {
  const char *eq = e; while (*eq && *eq != '=') eq++;
  u64 n = (u64)(eq - e);
  for (int i = 0; i < nunset; i++) if (slen(unsets[i]) == n && same_n(e, unsets[i], n)) return 1;
  if (n == 10 && (same_n(e, "LD_PRELOAD", 10) || same_n(e, "DN_INSTDIR", 10))) return 1;
  if (n == 15 && same_n(e, "LD_LIBRARY_PATH", 15)) return 1;
  if (has_rprefix && n == 20 && same_n(e, "DN_REDIRECT_PREFIXES", 20)) return 1;
  if (g_new_bio && n == 17 && same_n(e, "DN_BIONIC_PRELOAD", 17)) return 1;
  return env_is_set(e, n);
}

static void policy_defaults(void) {
  join_prefix(g_loader, DN_PATH_MAX, "/usr/lib/ld-linux-aarch64.so.1");
  join_prefix(g_default_pre, DN_PATH_MAX, "/usr/lib/deb-native/path-redirect.so");
  add_lib("/usr/lib/aarch64-linux-gnu");
  add_lib("/usr/lib");
  /* COMPILER_PATH: gcc's cc1/as/ld do not fall back to PATH (ld-dn.c
   * history); one of the generic env entries now, so a config can replace
   * it with `env COMPILER_PATH=...`. */
  set_env("COMPILER_PATH", "$DN/usr/bin", 0);
  /* Replaced by ld-dn itself or rebuilt below; drop the caller's copy. */
  add_unset("LD_PRELOAD"); add_unset("DN_INSTDIR");
  add_unset("LD_LIBRARY_PATH"); add_unset("COMPILER_PATH");
}

/* ---- config parser -------------------------------------------------- */

static char *skip_ws(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static void rtrim(char *s) {
  u64 n = slen(s);
  while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
}
static void do_unset(char *rest) {
  char *p = rest;
  while (*p) {
    char *t = skip_ws(p);
    if (!*t) break;
    char *e = t; while (*e && *e != ' ' && *e != '\t') e++;
    char save = *e; *e = 0;
    add_unset(t);
    p = save ? e + 1 : e;
  }
}
static void do_shimprefix(char *rest) {
  char *p = rest; int any = 0, bad = 0;
  char build[DN_PATH_MAX]; build[0] = 0;
  while (*p) {
    char *t = skip_ws(p);
    if (!*t) break;
    char *e = t; while (*e && *e != ' ' && *e != '\t') e++;
    char save = *e; *e = 0;
    if ((any && app(build, sizeof build, ":") != 0) || app(build, sizeof build, t) != 0) { bad = 1; break; }
    any = 1;
    p = save ? e + 1 : e;
  }
  if (bad) { warn("config: shim-prefix too long, ignored", 0); return; }
  if (any) { cat2(g_rprefix, sizeof g_rprefix, "", build); has_rprefix = 1; }
}
static void do_env(char *rest) {
  char *eq = rest; while (*eq && *eq != '=') eq++;
  if (!*eq) { warn("config: env needs KEY=VALUE", rest); return; }
  *eq = 0;
  set_env(rest, eq + 1, 1);
}
static void dispatch(char *s) {
  char *sp = s; while (*sp && *sp != ' ' && *sp != '\t') sp++;
  char dir[32]; u64 dl = (u64)(sp - s);
  if (dl >= sizeof dir) dl = sizeof dir - 1;
  memcpy(dir, s, dl); dir[dl] = 0;
  char *rest = skip_ws(sp);
  if (same(dir, "loader")) {
    if (!*rest) warn("config: loader needs a path", 0);
    else if (join_prefix(g_loader, DN_PATH_MAX, rest) != 0) warn("config: loader path too long, ignored", rest);
  } else if (same(dir, "lib-add")) {
    if (*rest) add_lib(rest); else warn("config: lib-add needs a path", 0);
  } else if (same(dir, "no-default-lib")) {
    nlib = 0;
  } else if (same(dir, "preload")) {
    if (*rest) add_pre(rest); else warn("config: preload needs a path", 0);
  } else if (same(dir, "no-default-preload")) {
    default_pre = 0;
  } else if (same(dir, "env")) {
    if (*rest) do_env(rest); else warn("config: env needs KEY=VALUE", 0);
  } else if (same(dir, "unset")) {
    do_unset(rest);
  } else if (same(dir, "shim-prefix")) {
    do_shimprefix(rest);
  } else {
    warn("config: unknown directive", dir);
  }
}
static int match_prog(const char *name) {
  if (name[0] == '/') return g_have_prog && ends_with(g_progexe, name);
  return same(g_progbase, name);
}
static void parse_config(char *b) {
  int in_prog = 0, active = 1;
  char *line = b;
  while (*line) {
    char *nl = line; while (*nl && *nl != '\n') nl++;
    char save = *nl; *nl = 0;
    char *s = line;
    for (char *c = s; *c; c++) if (*c == '#') { *c = 0; break; }
    s = skip_ws(s); rtrim(s);
    if (*s == '[') {
      char *e = s + 1; while (*e && *e != ']') e++;
      if (*e == ']') { *e = 0; in_prog = 1; active = match_prog(s + 1); }
      else warn("config: unclosed program block", s);
    } else if (*s && (!in_prog || active)) {
      dispatch(s);
    }
    line = nl + (save ? 1 : 0);
  }
}
static void program_name(char **argv) {
  g_have_prog = 0;
  i64 n = sys6(SYS_readlinkat, AT_FDCWD, (i64)"/proc/self/exe", (i64)g_progexe, DN_PATH_MAX - 1, 0, 0);
  if (n > 0) { g_progexe[n] = 0; g_have_prog = 1; }
  else if (argv && argv[0]) {
    u64 l = slen(argv[0]); if (l >= DN_PATH_MAX) l = DN_PATH_MAX - 1;
    memcpy(g_progexe, argv[0], l); g_progexe[l] = 0; g_have_prog = 1;
  }
  const char *b = g_progexe;
  for (const char *p = g_progexe; *p; p++) if (*p == '/') b = p + 1;
  u64 bl = slen(b); if (bl >= sizeof g_progbase) bl = sizeof g_progbase - 1;
  memcpy(g_progbase, b, bl); g_progbase[bl] = 0;
}
static void load_config(const char *override) {
  char path[DN_PATH_MAX];
  if (override && *override) {
    if (slen(override) + 1 > DN_PATH_MAX) { warn("DN_CONFIG too long, ignored", 0); return; }
    memcpy(path, override, slen(override) + 1);
  } else {
    cat2(path, DN_PATH_MAX, g_dn, "/etc/deb-native/ld-dn.conf");
  }
  i64 fd = sys6(SYS_openat, AT_FDCWD, (i64)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
  if (failed(fd)) return;   /* ENOENT: no config, defaults stand */
  i64 n = sys6(SYS_read, fd, (i64)cfg, DN_CFG_MAX - 1, 0, 0, 0);
  sys6(SYS_close, fd, 0, 0, 0, 0, 0);
  if (n < 0) { warn("config: read failed", path); return; }
  if (n >= DN_CFG_MAX - 1) { warn("config: too large, ignored", path); return; }
  cfg[n] = 0;
  parse_config(cfg);
}

/* ---- emit ----------------------------------------------------------- */

/* Is tok (length tl) already one of the ':'-separated entries in val? */
static int lib_has(const char *val, const char *tok, u64 tl) {
  const char *p = val;
  while (*p) {
    const char *e = p; while (*e && *e != ':') e++;
    u64 l = (u64)(e - p);
    if (l == tl && same_n(p, tok, l)) return 1;
    p = *e ? e + 1 : e;
  }
  return 0;
}
/* Append tok to the value of a "LD_LIBRARY_PATH=..." string, unless it is
 * already there. */
static int append_lib(char *dst, u64 cap, const char *tok, u64 tl) {
  const char *val = dst + sizeof("LD_LIBRARY_PATH=") - 1;
  if (lib_has(val, tok, tl)) return 0;
  if (*val && app(dst, cap, ":") != 0) return -1;
  return app(dst, cap, tok);
}

static void build_env(void) {
  cat2(g_env_pre, sizeof g_env_pre, "LD_PRELOAD=", "");
  int first = 1, over = 0;
  if (default_pre) { over = app(g_env_pre, sizeof g_env_pre, g_default_pre) != 0; if (!over) first = 0; }
  for (int i = 0; i < npre && !over; i++) {
    if (!first) over = app(g_env_pre, sizeof g_env_pre, ":") != 0;
    if (!over) over = app(g_env_pre, sizeof g_env_pre, pres[i]) != 0;
    if (!over) first = 0;
  }
  if (!over && g_extra_preload && *g_extra_preload) {
    if (!first) over = app(g_env_pre, sizeof g_env_pre, ":") != 0;
    if (!over) over = app(g_env_pre, sizeof g_env_pre, g_extra_preload) != 0;
  }
  if (over) warn("config: preload list too long, truncated", 0);

  cat2(g_env_inst, sizeof g_env_inst, "DN_INSTDIR=", g_dn);

  /* The standard glibc mechanism is honoured, not discarded: the fixed
   * dirs first (so the prefix's own libraries always win), then the
   * caller's LD_LIBRARY_PATH entries, deduplicated. That makes
   * `LD_LIBRARY_PATH=... prog` behave the way a glibc developer expects,
   * with no project-specific escape hatch (docs/guides/gcc-glibc-dev.md). */
  cat2(g_env_lib, sizeof g_env_lib, "LD_LIBRARY_PATH=", "");
  int libover = 0;
  for (int i = 0; i < nlib && !libover; i++)
    if (append_lib(g_env_lib, sizeof g_env_lib, libs[i], slen(libs[i])) != 0) libover = 1;
  if (g_inherited_lib && *g_inherited_lib) {
    const char *p = g_inherited_lib;
    while (*p && !libover) {
      const char *e = p;
      while (*e && *e != ':') e++;
      u64 l = (u64)(e - p);
      if (l && l < DN_PATH_MAX) {
        char tok[DN_PATH_MAX];
        memcpy(tok, p, l); tok[l] = 0;
        if (append_lib(g_env_lib, sizeof g_env_lib, tok, l) != 0) libover = 1;
      }
      p = *e ? e + 1 : e;
    }
  }
  if (libover) warn("config: library list too long, truncated", 0);

  if (has_rprefix) cat2(g_env_rprefix, sizeof g_env_rprefix, "DN_REDIRECT_PREFIXES=", g_rprefix);
}
static void dump_policy(void) {
  put("ld-dn " DN_VERSION "\n");
  put("  prefix: "); put(g_dn); put("\n");
  put("  loader: "); put(g_loader); put("\n");
  for (int i = 0; i < nlib; i++) { put("  lib-add: "); put(libs[i]); put("\n"); }
  if (g_inherited_lib && *g_inherited_lib) { put("  lib-env: "); put(g_inherited_lib); put("\n"); }
  put("  preload:"); put(default_pre ? " " : " (no-default)");
  if (default_pre) put(g_default_pre);
  for (int i = 0; i < npre; i++) { put(" "); put(pres[i]); }
  put("\n");
  for (int i = 0; i < nset; i++) { put("  env: "); put(sets[i]); put("\n"); }
  put("  unset:"); for (int i = 0; i < nunset; i++) { put(" "); put(unsets[i]); } put("\n");
  if (has_rprefix) { put("  shim-prefix: "); put(g_rprefix); put("\n"); }
}

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

  /* 2. Policy: compiled defaults, config file, then one-shot overrides. */
  const char *inherited = findenv(envp, nenv, "LD_PRELOAD");
  const char *cfg_override = findenv(envp, nenv, "DN_CONFIG");
  g_inherited_lib = findenv(envp, nenv, "LD_LIBRARY_PATH");
  g_extra_preload = findenv(envp, nenv, "DN_PRELOAD");
  g_new_bio = inherited && *inherited && !has(inherited, "path-redirect.so");
  if (g_new_bio) {
    if (slen(inherited) + sizeof("DN_BIONIC_PRELOAD=") - 1 < sizeof g_env_bio)
      cat2(g_env_bio, sizeof g_env_bio, "DN_BIONIC_PRELOAD=", inherited);
    else { warn("inherited LD_PRELOAD too long, DN_BIONIC_PRELOAD dropped", 0); g_new_bio = 0; }
  }

  program_name(argv);
  policy_defaults();
  if (!findenv(envp, nenv, "DN_NO_CONFIG")) load_config(cfg_override);
  build_env();
  if (findenv(envp, nenv, "DN_REDIRECT_DEBUG")) dump_policy();

  /* 3. The new stack: argc, argv, kept envp + ours, auxv -- in the space
   * _start reserved just below the original stack. */
  long kept = 0;
  for (long i = 0; i < nenv; i++) if (!skip_env(envp[i])) kept++;
  long nextra = 3 + (has_rprefix ? 1 : 0) + nset + (g_new_bio ? 1 : 0);
  u64 words = 1 + (u64)argc + 1 + (u64)kept + (u64)nextra + 1 + 2 * ((u64)naux + 1);
  if (words * 8 + 64 > RESERVE) die("environment too large", 0);
  u64 *ns = (u64 *)(((u64)sp - words * 8) & ~(u64)15);
  u64 k = 0;
  ns[k++] = (u64)argc;
  for (long i = 0; i < argc; i++) ns[k++] = (u64)argv[i];
  ns[k++] = 0;
  for (long i = 0; i < nenv; i++) if (!skip_env(envp[i])) ns[k++] = (u64)envp[i];
  ns[k++] = (u64)g_env_pre;
  ns[k++] = (u64)g_env_inst;
  ns[k++] = (u64)g_env_lib;
  if (has_rprefix) ns[k++] = (u64)g_env_rprefix;
  for (int i = 0; i < nset; i++) ns[k++] = (u64)sets[i];
  if (g_new_bio) ns[k++] = (u64)g_env_bio;
  ns[k++] = 0;
  u64 *nauxv = ns + k;

  /* 4. Map glibc's real loader. */
  i64 fd = sys6(SYS_openat, AT_FDCWD, (i64)g_loader, O_RDONLY | O_CLOEXEC, 0, 0, 0);
  if (failed(fd)) die("cannot open", g_loader);
  Ehdr eh;
  if (sys6(SYS_pread64, fd, (i64)&eh, sizeof eh, 0, 0, 0) != (i64)sizeof eh ||
      eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_machine != 183 ||
      eh.e_phnum > sizeof g_ph / sizeof g_ph[0])
    die("not an arm64 ELF loader", g_loader);
  if (sys6(SYS_pread64, fd, (i64)g_ph, eh.e_phnum * sizeof(Phdr), (i64)eh.e_phoff, 0, 0) != (i64)(eh.e_phnum * sizeof(Phdr)))
    die("cannot read program headers", g_loader);
  u64 pm = pagesz - 1, lo = ~(u64)0, hi = 0;
  for (int i = 0; i < eh.e_phnum; i++) {
    if (g_ph[i].p_type != PT_LOAD) continue;
    if ((g_ph[i].p_vaddr & ~pm) < lo) lo = g_ph[i].p_vaddr & ~pm;
    if (((g_ph[i].p_vaddr + g_ph[i].p_memsz + pm) & ~pm) > hi) hi = (g_ph[i].p_vaddr + g_ph[i].p_memsz + pm) & ~pm;
  }
  i64 area = sys6(SYS_mmap, 0, (i64)(hi - lo), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (failed(area)) die("cannot reserve memory for", g_loader);
  u64 base = (u64)area - lo;
  for (int i = 0; i < eh.e_phnum; i++) {
    Phdr *s = &g_ph[i];
    if (s->p_type != PT_LOAD) continue;
    int prot = (s->p_flags & 4 ? PROT_READ : 0) | (s->p_flags & 2 ? PROT_WRITE : 0) | (s->p_flags & 1 ? PROT_EXEC : 0);
    u64 start = s->p_vaddr & ~pm, fend = s->p_vaddr + s->p_filesz, fmapend = (fend + pm) & ~pm;
    u64 mend = (s->p_vaddr + s->p_memsz + pm) & ~pm;
    if (s->p_filesz) {
      i64 r = sys6(SYS_mmap, (i64)(base + start), (i64)(fmapend - start), prot, MAP_PRIVATE | MAP_FIXED, fd, (i64)(s->p_offset & ~pm));
      if (failed(r)) die("cannot map a segment of", g_loader);
    }
    if (s->p_memsz > s->p_filesz) {
      if ((prot & PROT_WRITE) && fend < fmapend && s->p_filesz) memset((void *)(base + fend), 0, fmapend - fend);
      u64 zstart = s->p_filesz ? fmapend : start;
      if (mend > zstart) {
        i64 r = sys6(SYS_mmap, (i64)(base + zstart), (i64)(mend - zstart), prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        if (failed(r)) die("cannot map the zeroed part of", g_loader);
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
