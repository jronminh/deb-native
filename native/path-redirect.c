/* "Manual overlay" — the middleman between a package's hardcoded absolute
 * paths and where those files actually live on Termux, without any of
 * sudo-less's kernel mechanisms (mount namespace + overlayfs).
 *
 * This is an LD_PRELOAD shared library: it intercepts the libc calls a
 * dynamically-linked glibc binary uses to open/stat/exec/create a file and
 * rewrites any path under /usr, /etc, /var or /opt to the same path under
 * DN_INSTDIR before handing it to the real libc function.
 *
 * Two things changed after on-device testing showed the first version was
 * too narrow:
 *
 * 1. The interception set was grown well past open/stat/exec. Maintainer
 *    scripts run under a real shell and fork real coreutils; `mkdir -p
 *    /var/lib/foo` and `ln -s`, `rm`, `mv`, `chmod`, `touch`, `ls` all go
 *    through mkdirat/unlinkat/symlinkat/renameat/utimensat/statx, none of
 *    which were covered. A command that isn't intercepted falls through to
 *    the real root and fails with EROFS ("cannot create directory '/var':
 *    Read-only file system") or ENOENT.
 *
 * 2. execve() is now a dispatch point, not just a rewrite. The shell itself
 *    is glibc and can load this shim; a forked *Bionic* command (Termux's
 *    sed/grep/awk, /system/bin/sh) cannot — Bionic's linker aborts with
 *    "CANNOT LINK EXECUTABLE ... library libc.so.6 not found" if this glibc
 *    .so is in its LD_PRELOAD. So on execve we inspect the target: keep
 *    LD_PRELOAD for a glibc target (it is safe and wants the redirect),
 *    strip it for anything else. A script whose interpreter is a glibc
 *    shell/perl is exec'd through the interpreter explicitly, because the
 *    kernel resolves a shebang itself and never gives this shim a chance to
 *    redirect the *interpreter* path (/bin/sh exists on Android as root's
 *    toybox; /usr/bin/perl does not exist at all).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <elf.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/xattr.h>
#include <utime.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <mntent.h>
#include <grp.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>

/* Cached once, at load. rewrite() is on the hot path of every intercepted
 * open/stat/exec call, so it must not call getenv()/strlen() per call or
 * snprintf() to build the result -- a plain branch + memcpy is enough and
 * measurably faster (see docs/log/findings.md). The env is
 * fixed before exec by the launchers, or derived from the shim's own load
 * path when nothing injects env (fused loader); caching is safe either way. */
static const char *g_root;
static size_t g_rootlen;
static int g_debug;
static const char *g_bionic_preload;
static int g_init;
/* Redirect roots a caller can set (DN_REDIRECT_PREFIXES) to replace the
 * compiled set with: ':'-separated guest paths, e.g.
 * "/usr:/etc:/var". Unset keeps the switch-based default below, with no
 * per-call list walk. */
static char g_rprefixes[512];
static int g_rprefixes_custom;

/* Fake root (since 0.2.3): inside the prefix a program sees itself as
 * root, as on a Debian where apt, dpkg and maintainer scripts run as root
 * and base-passwd's only user is root (HOME=/root is already Termux's
 * home). Only the identity is faked -- nothing gains a right it did not
 * have. What a program learns its identity from, and the fake:
 *   - get[e]uid/get[e]gid/getres[ug]id/getgroups -> 0;
 *   - stat()'s owner: files owned by the real uid/gid show as root's
 *     (else git's "dubious ownership", ssh's "bad owner" once uid is 0);
 *   - set*id()/setgroups() and chown() to someone else "succeed" (nothing
 *     is recorded: a chowned file still shows as root's);
 *   - USER/LOGNAME = root.
 * DN_ID=user turns it off for one command and its children (programs that
 * refuse root: postgres, Chromium's sandbox). Static programs under
 * dn-trace do not get it (the shim is not loaded there). */
static int g_fakeroot;
static uid_t g_ruid;
static gid_t g_rgid;

/* Set NAME=VALUE in the environ array itself: a program's main() often
 * reads its envp (bash does), which setenv() would leave behind when it
 * copies the array. */
extern char **environ;
static void fake_env(const char *name, char *entry) {
  size_t n = strlen(name);
  for (char **e = environ; e && *e; e++)
    if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') { *e = entry; return; }
  setenv(name, entry + n + 1, 1);
}

static void dn_init(void) {
  g_root = getenv("DN_INSTDIR");
  if (g_root && !*g_root) g_root = NULL;
  if (!g_root) {
    /* Nothing injected DN_INSTDIR (fused-loader mode): derive the prefix
     * from this shim's own load path, which is
     * <prefix>/usr/lib/deb-native/path-redirect.so wherever it is listed
     * (ld.so.preload / LD_PRELOAD). dladdr() reports that path. */
    static const char suffix[] = "/usr/lib/deb-native/path-redirect.so";
    static char rootbuf[4096];
    Dl_info info;
    if (dladdr((void *)&dn_init, &info) && info.dli_fname) {
      size_t lp = strlen(info.dli_fname), ls = sizeof suffix - 1;
      if (lp > ls && memcmp(info.dli_fname + lp - ls, suffix, ls) == 0) {
        memcpy(rootbuf, info.dli_fname, lp - ls);
        rootbuf[lp - ls] = 0;
        g_root = rootbuf;
      }
    }
  }
  g_rootlen = g_root ? strlen(g_root) : 0;
  g_debug = getenv("DN_REDIRECT_DEBUG") != NULL;
  if (g_debug)
    fprintf(stderr, "[path-redirect] root=%s\n", g_root ? g_root : "(null)");
  g_bionic_preload = getenv("DN_BIONIC_PRELOAD");
  const char *rp = getenv("DN_REDIRECT_PREFIXES");
  if (rp && *rp) {
    size_t n = strlen(rp);
    if (n >= sizeof g_rprefixes) n = sizeof g_rprefixes - 1;
    memcpy(g_rprefixes, rp, n);
    g_rprefixes[n] = 0;
    g_rprefixes_custom = 1;
  }
  const char *id = getenv("DN_ID");
  g_fakeroot = g_root && !(id && strcmp(id, "user") == 0);
  g_ruid = (uid_t)syscall(SYS_getuid);
  g_rgid = (gid_t)syscall(SYS_getgid);
  if (g_fakeroot) {
    /* Writable: bash edits environment entries in place. */
    static char user[] = "USER=root", logname[] = "LOGNAME=root";
    fake_env("USER", user);
    fake_env("LOGNAME", logname);
  }
  g_init = 1;
}
__attribute__((constructor)) static void dn_ctor(void) { dn_init(); }

/* The owner a stat() result shows under fake root. */
#define FAKE_OWNER(st) do {                                          \
    if (g_fakeroot) {                                                \
      if ((st)->st_uid == g_ruid) (st)->st_uid = 0;                  \
      if ((st)->st_gid == g_rgid) (st)->st_gid = 0;                  \
    }                                                                \
  } while (0)
#define FAKE_OWNER_X(stx) do {                                       \
    if (g_fakeroot) {                                                \
      if ((stx)->stx_uid == g_ruid) (stx)->stx_uid = 0;              \
      if ((stx)->stx_gid == g_rgid) (stx)->stx_gid = 0;              \
    }                                                                \
  } while (0)
/* A chown()/set*id() that fails only for lack of rights "succeeds". */
#define FAKE_OK(r) do {                                              \
    if ((r) != 0 && g_fakeroot && errno == EPERM) { errno = 0; return 0; } \
    return (r);                                                      \
  } while (0)

static const char *rewrite(const char *path, char *buf, size_t bufsz) {
  if (!g_init) dn_init();
  if (!g_root || !path || path[0] != '/') return path;

  /* Only /usr, /etc, /var, /opt, /root, /run, /tmp, /lib, /bin and /sbin
   * qualify; dispatch on the second byte so a non-matching path costs one
   * compare instead of many strncmp()s. /root: the prefix's root links to
   * Termux's home. /run, /tmp: guest runtime/temp state the prefix owns, so
   * programs that hardcode /tmp (Android has none writable) or /run stay in
   * the prefix. /lib, /bin, /sbin: Debian's merged-usr symlinks into
   * /usr/{lib,bin,sbin} (base-files creates the same symlinks inside the
   * prefix) -- without this, anything that hardcodes the non-merged path
   * (e.g. libc6-dev's /lib/<triplet>/libc.so linker script) misses the
   * prefix entirely instead of following the symlink (docs/log/findings.md,
   * 2026-10-01). /dev, /proc, /sys are deliberately REAL (kernel/device). */
  size_t prelen;
  if (g_rprefixes_custom) {
    prelen = 0;
    const char *p = g_rprefixes;
    while (*p) {
      const char *e = p;
      while (*e && *e != ':') e++;
      size_t l = (size_t)(e - p);
      if (l && strncmp(path, p, l) == 0 && (path[l] == '/' || path[l] == '\0')) { prelen = l; break; }
      p = *e ? e + 1 : e;
    }
  } else {
    const char *pre;
    prelen = 4;
    switch (path[1]) {
      case 'u': pre = "/usr"; break;
      case 'e': pre = "/etc"; break;
      case 'v': pre = "/var"; break;
      case 'o': pre = "/opt"; break;
      case 'r':
        if (!strncmp(path, "/root", 5)) { pre = "/root"; prelen = 5; }
        else { pre = "/run"; prelen = 4; }
        break;
      case 't': pre = "/tmp"; break;
      case 'l': pre = "/lib"; break;
      case 'b': pre = "/bin"; break;
      case 's': pre = "/sbin"; prelen = 5; break;
      default: return path;
    }
    if (strncmp(path, pre, prelen) != 0) return path;
  }
  if (prelen == 0 || (path[prelen] != '/' && path[prelen] != '\0')) return path;

  size_t plen = strlen(path);
  if (g_rootlen + plen + 1 > bufsz) return path;
  memcpy(buf, g_root, g_rootlen);
  memcpy(buf + g_rootlen, path, plen + 1);
  if (g_debug) fprintf(stderr, "[path-redirect] %s -> %s\n", path, buf);
  return buf;
}

/* ---- execve dispatch ------------------------------------------------- */

/* Environment for a NON-glibc (Bionic/script) child.
 *
 * A glibc LD_PRELOAD must never reach a Bionic process: Bionic's linker
 * aborts ("library libc.so.6 not found"). But simply dropping LD_PRELOAD
 * would also drop Termux's own termux-exec preload, which Termux-native
 * commands rely on for shebang handling -- and disabling termux-exec
 * system-wide can break Termux packages, so we must not.
 *
 * The launcher/wrapper captures whatever LD_PRELOAD it inherited (i.e.
 * termux-exec) into DN_BIONIC_PRELOAD before installing the path-redirect
 * shim. Here we put that back for a Bionic child; if there was none (e.g.
 * running under dpkg, which has no preload), LD_PRELOAD is removed.
 */
static char **bionic_env(char *const *envp) {
  if (!envp) return (char **)envp;
  if (!g_init) dn_init();
  const char *b = g_bionic_preload;
  int want = (b && *b);
  static char entry[8192];
  if (want) snprintf(entry, sizeof entry, "LD_PRELOAD=%s", b);
  static char *out[2048];
  /* PATH, too: dn-shell puts the prefix's and Termux's glibc tools ahead of
   * Termux's Bionic ones for glibc scripts, but a Bionic child (Termux's
   * dpkg-realpath, a #!/system/bin/sh wrapper) carries termux-exec's Bionic
   * preload, and a glibc tool it runs by name (Debian's basename) dies
   * loading it ("libc.so: invalid ELF header"). Hand it the same PATH with
   * every prefix directory and every .../glibc/bin moved last. */
  static char path_entry[16384];
  int n = 0, saw = 0;
  for (char *const *e = envp; *e && n < 2045; e++) {
    if (!strncmp(*e, "LD_PRELOAD=", 11)) {
      saw = 1;
      if (want) out[n++] = entry;
      continue;
    }
    /* Same reasoning as LD_PRELOAD above, found 2026-09-30 root-causing a
     * real crash: a glibc launch sets LD_LIBRARY_PATH (and COMPILER_PATH)
     * pointing at this project's own glibc library/bin dirs -- a Bionic
     * child inheriting that had no
     * business seeing it, but bionic_env() only ever stripped LD_PRELOAD.
     * Confirmed directly: Termux's own dpkg-deb (Bionic) fails to link
     * ("cannot find verneed/verdef ... at .../glibc/lib/libc.so.6") when
     * launched with the prefix's LD_LIBRARY_PATH still set -- Bionic's
     * linker partially honors it too, and finds glibc's libc.so.6 where
     * it expects its own. Drop both unconditionally for a Bionic child. */
    if (!strncmp(*e, "LD_LIBRARY_PATH=", 16) || !strncmp(*e, "COMPILER_PATH=", 14)) continue;
    if (!strncmp(*e, "PATH=", 5) && (strstr(*e, "/glibc/bin") || (g_root && strstr(*e, g_root)))) {
      char tail[16384] = "";
      size_t hl = 0, tl = 0;
      const char *p = *e + 5;
      memcpy(path_entry, "PATH=", 5); hl = 5;
      while (*p) {
        const char *c = strchr(p, ':');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        int glibc = (len >= 10 && !memcmp(p + len - 10, "/glibc/bin", 10)) ||
                    (g_root && len >= g_rootlen && !memcmp(p, g_root, g_rootlen) &&
                     (len == g_rootlen || p[g_rootlen] == '/'));
        if (glibc) {
          if (tl + len + 2 < sizeof tail) { if (tl) tail[tl++] = ':'; memcpy(tail + tl, p, len); tl += len; tail[tl] = 0; }
        } else if (hl + len + 2 < sizeof path_entry) {
          if (hl > 5) path_entry[hl++] = ':';
          memcpy(path_entry + hl, p, len); hl += len;
        }
        p += len + (c ? 1 : 0);
      }
      if (tl && hl + tl + 2 < sizeof path_entry) { if (hl > 5) path_entry[hl++] = ':'; memcpy(path_entry + hl, tail, tl); hl += tl; }
      path_entry[hl] = 0;
      out[n++] = path_entry;
      continue;
    }
    out[n++] = *e;
  }
  if (want && !saw && n < 2045) out[n++] = entry;
  out[n] = NULL;
  if (g_debug) {
    fprintf(stderr, "[path-redirect] bionic_env: %d entries out:\n", n);
    for (int i = 0; i < n; i++)
      if (!strncmp(out[i], "LD_", 3) || !strncmp(out[i], "PATH=", 5) || !strncmp(out[i], "COMPILER_PATH", 13))
        fprintf(stderr, "[path-redirect]   %s\n", out[i]);
  }
  return out;
}

/* Returns 1 and fills interp[] if path is an ELF whose PT_INTERP is a glibc
 * ld-linux; 0 otherwise (Bionic, static, not an ELF, unreadable). */
static int elf_glibc_interp(int fd, const unsigned char *hdr, ssize_t n) {
  if (n < (ssize_t)sizeof(Elf64_Ehdr)) return 0;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)hdr;
  if (eh->e_phentsize != sizeof(Elf64_Phdr)) return 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    Elf64_Phdr ph;
    off_t off = (off_t)eh->e_phoff + (off_t)i * sizeof ph;
    if (pread(fd, &ph, sizeof ph, off) != (ssize_t)sizeof ph) return 0;
    if (ph.p_type != PT_INTERP || ph.p_filesz == 0 || ph.p_filesz > 512) continue;
    char interp[512];
    ssize_t r = pread(fd, interp, ph.p_filesz, ph.p_offset);
    if (r <= 0) return 0;
    interp[(r < (ssize_t)sizeof interp) ? r : (ssize_t)sizeof interp - 1] = '\0';
    /* Every program this project has translated has its PT_INTERP set to
     * the prefix's own fused glibc loader (dn-translate-deb.sh/dn-adopt.sh,
     * patchelf --set-interpreter), an "ld-linux" path -- caught by the
     * ld-linux match below. A glibc target misclassified as Bionic would
     * get bionic_env()'s PATH-reordering (meant for a genuinely Bionic
     * child) applied to it, which can point its own PATH lookups at
     * Termux's own binaries instead of the prefix's (found 2026-09-30
     * root-causing a segfault: find -exec test / env test). */
    return strstr(interp, "ld-linux") != NULL && strstr(interp, "glibc") != NULL
               ? 1
               : (strstr(interp, "/glibc/") != NULL || strstr(interp, "ld-linux") != NULL);
  }
  return 0;
}

static int target_is_glibc(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  unsigned char hdr[1024];
  ssize_t n = read(fd, hdr, sizeof hdr);
  if (n >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
    int r = elf_glibc_interp(fd, hdr, n);
    close(fd);
    return r;
  }
  close(fd);
  return 0;
}

/* If path is a `#!interp [arg]` script, copy interp and arg (may be empty)
 * and return 1. */
static int target_is_script(const char *path, char *interp, size_t isz,
                            char *arg, size_t asz) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char line[512];
  ssize_t n = read(fd, line, sizeof line - 1);
  close(fd);
  if (n < 2 || line[0] != '#' || line[1] != '!') return 0;
  line[n] = '\0';
  char *p = line + 2;
  while (*p == ' ') p++;
  char *end = p;
  while (*end && *end != '\n' && *end != ' ' && *end != '\t') end++;
  size_t l = (size_t)(end - p);
  if (l == 0 || l >= isz) return 0;
  memcpy(interp, p, l);
  interp[l] = '\0';
  arg[0] = '\0';
  if (*end == ' ' || *end == '\t') {
    char *a = end + 1;
    while (*a == ' ' || *a == '\t') a++;
    char *ae = a;
    while (*ae && *ae != '\n' && *ae != ' ' && *ae != '\t') ae++;
    size_t al = (size_t)(ae - a);
    if (al && al < asz) { memcpy(arg, a, al); arg[al] = '\0'; }
  }
  return 1;
}

static const char *map_shebang_interp(const char *in, char *buf, size_t sz) {
  if (!g_init) dn_init();
  const char *root = g_root;
  if (root) {
    /* Same preference as dn-translate-deb.sh/patch-scripts-tree.sh
     * (translate: direct shebang, 2026-09-30): point at the prefix's own
     * dash/bash directly when installed -- real apt packages with the fused
     * loader as their own ELF interpreter, so the kernel following the rewritten
     * shebang already gets the shim/environment set up, no extra
     * indirection needed. dn-shell is only the bootstrap-time fallback,
     * kept here too for the same chicken-and-egg reason (a script the
     * shim encounters live, e.g. via system()/posix_spawn, before
     * dash/bash are installed). Runtime component audit item 4,
     * 2026-09-30 -- this was the one remaining place still hardcoded to
     * dn-shell unconditionally after the translate-time scripts were
     * fixed. */
    if (!strcmp(in, "/bin/sh") || !strcmp(in, "/bin/dash") ||
        !strcmp(in, "/usr/bin/sh") || !strcmp(in, "/usr/bin/dash")) {
      snprintf(buf, sz, "%s/usr/bin/dash", root);
      if (access(buf, X_OK) == 0) return buf;
      snprintf(buf, sz, "%s/usr/bin/dn-shell", root);
      return buf;
    }
    if (!strcmp(in, "/bin/bash") || !strcmp(in, "/usr/bin/bash")) {
      snprintf(buf, sz, "%s/usr/bin/bash", root);
      if (access(buf, X_OK) == 0) return buf;
      snprintf(buf, sz, "%s/usr/bin/dn-shell", root);
      return buf;
    }
    if (!strncmp(in, "/usr/bin/perl", 13) || !strncmp(in, "/bin/perl", 9)) {
      snprintf(buf, sz, "%s/usr/bin/dn-perl", root);
      return buf;
    }
  }
  return rewrite(in, buf, sz);
}

/* ---- open/stat family ------------------------------------------------ */

typedef int (*open64_t)(const char *, int, ...);
int open64(const char *pathname, int flags, ...) {
  static open64_t real;
  if (!real) real = (open64_t)dlsym(RTLD_NEXT, "open64");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(rewrite(pathname, buf, sizeof buf), flags, mode);
}

typedef int (*openat_t)(int, const char *, int, ...);
int openat(int dirfd, const char *pathname, int flags, ...) {
  static openat_t real;
  if (!real) real = (openat_t)dlsym(RTLD_NEXT, "openat");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags, mode);
}

typedef int (*open_t)(const char *, int, ...);
int open(const char *pathname, int flags, ...) {
  static open_t real;
  if (!real) real = (open_t)dlsym(RTLD_NEXT, "open");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(rewrite(pathname, buf, sizeof buf), flags, mode);
}

/* Fortified variants: with _FORTIFY_SOURCE the compiler can bind a call
 * directly to __open_2/__openat_2/__open64_2, bypassing the interposable
 * open/openat/open64 symbols. */
typedef int (*__open_2_t)(const char *, int);
int __open_2(const char *pathname, int flags) {
  static __open_2_t real;
  if (!real) real = (__open_2_t)dlsym(RTLD_NEXT, "__open_2");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), flags);
}

typedef int (*__openat_2_t)(int, const char *, int);
int __openat_2(int dirfd, const char *pathname, int flags) {
  static __openat_2_t real;
  if (!real) real = (__openat_2_t)dlsym(RTLD_NEXT, "__openat_2");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags);
}

typedef int (*__open64_2_t)(const char *, int);
int __open64_2(const char *pathname, int flags) {
  static __open64_2_t real;
  if (!real) real = (__open64_2_t)dlsym(RTLD_NEXT, "__open64_2");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), flags);
}

typedef int (*fstatat_t)(int, const char *, struct stat *, int);
int fstatat(int dirfd, const char *pathname, struct stat *st, int flags) {
  static fstatat_t real;
  if (!real) real = (fstatat_t)dlsym(RTLD_NEXT, "fstatat");
  char buf[4096];
  int r = real(dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*fxstatat_t)(int, int, const char *, struct stat *, int);
int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *st, int flags) {
  static fxstatat_t real;
  if (!real) real = (fxstatat_t)dlsym(RTLD_NEXT, "__fxstatat");
  if (!real) return -1;
  char buf[4096];
  int r = real(ver, dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

/* Legacy stat entry points: a binary built against glibc < 2.33 reaches
 * stat through the versioned __xstat/__lxstat names, and Termux's glibc
 * still exports them for compatibility. The first argument is the (unused)
 * _STAT_VER. __fxstat takes an fd, not a path, so it needs no redirect. */
typedef int (*xstat_t)(int, const char *, struct stat *);
int __xstat(int ver, const char *pathname, struct stat *st) {
  static xstat_t real;
  if (!real) real = (xstat_t)dlsym(RTLD_NEXT, "__xstat");
  if (!real) return -1;
  char buf[4096];
  int r = real(ver, rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

int __lxstat(int ver, const char *pathname, struct stat *st) {
  static xstat_t real;
  if (!real) real = (xstat_t)dlsym(RTLD_NEXT, "__lxstat");
  if (!real) return -1;
  char buf[4096];
  int r = real(ver, rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*xstat64_t)(int, const char *, struct stat64 *);
int __xstat64(int ver, const char *pathname, struct stat64 *st) {
  static xstat64_t real;
  if (!real) real = (xstat64_t)dlsym(RTLD_NEXT, "__xstat64");
  if (!real) return -1;
  char buf[4096];
  int r = real(ver, rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

int __lxstat64(int ver, const char *pathname, struct stat64 *st) {
  static xstat64_t real;
  if (!real) real = (xstat64_t)dlsym(RTLD_NEXT, "__lxstat64");
  if (!real) return -1;
  char buf[4096];
  int r = real(ver, rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*stat_t)(const char *, struct stat *);
int stat(const char *pathname, struct stat *st) {
  static stat_t real;
  if (!real) real = (stat_t)dlsym(RTLD_NEXT, "stat");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*stat64_t)(const char *, struct stat64 *);
int stat64(const char *pathname, struct stat64 *st) {
  static stat64_t real;
  if (!real) real = (stat64_t)dlsym(RTLD_NEXT, "stat64");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

int lstat64(const char *pathname, struct stat64 *st) {
  static stat64_t real;
  if (!real) real = (stat64_t)dlsym(RTLD_NEXT, "lstat64");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*lstat_t)(const char *, struct stat *);
int lstat(const char *pathname, struct stat *st) {
  static lstat_t real;
  if (!real) real = (lstat_t)dlsym(RTLD_NEXT, "lstat");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*statx_t)(int, const char *, int, unsigned int, struct statx *);
int statx(int dirfd, const char *pathname, int flags, unsigned int mask,
          struct statx *stx) {
  static statx_t real;
  if (!real) real = (statx_t)dlsym(RTLD_NEXT, "statx");
  if (!real) return -1;
  char buf[4096];
  int r = real(dirfd, rewrite(pathname, buf, sizeof buf), flags, mask, stx);
  if (r == 0) FAKE_OWNER_X(stx);
  return r;
}

typedef int (*statfs_t)(const char *, struct statfs *);
int statfs(const char *pathname, struct statfs *buf) {
  static statfs_t real;
  if (!real) real = (statfs_t)dlsym(RTLD_NEXT, "statfs");
  char path[4096];
  return real(rewrite(pathname, path, sizeof path), buf);
}

typedef int (*statvfs_t)(const char *, struct statvfs *);
int statvfs(const char *pathname, struct statvfs *buf) {
  static statvfs_t real;
  if (!real) real = (statvfs_t)dlsym(RTLD_NEXT, "statvfs");
  char path[4096];
  return real(rewrite(pathname, path, sizeof path), buf);
}

typedef FILE *(*fopen_t)(const char *, const char *);
FILE *fopen(const char *pathname, const char *mode) {
  static fopen_t real;
  if (!real) real = (fopen_t)dlsym(RTLD_NEXT, "fopen");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef DIR *(*opendir_t)(const char *);
DIR *opendir(const char *pathname) {
  static opendir_t real;
  if (!real) real = (opendir_t)dlsym(RTLD_NEXT, "opendir");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf));
}

/* scandir()/scandir64() take the directory path and walk it themselves;
 * glibc uses an internal opendir that does not reach the interposed
 * symbol, so rewrite the path here too. */
typedef int (*scandir_t)(const char *, struct dirent ***,
                         int (*)(const struct dirent *),
                         int (*)(const struct dirent **, const struct dirent **));
int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  static scandir_t real;
  if (!real) real = (scandir_t)dlsym(RTLD_NEXT, "scandir");
  char buf[4096];
  return real(rewrite(dirp, buf, sizeof buf), namelist, filter, compar);
}

typedef int (*scandir64_t)(const char *, struct dirent64 ***,
                           int (*)(const struct dirent64 *),
                           int (*)(const struct dirent64 **, const struct dirent64 **));
int scandir64(const char *dirp, struct dirent64 ***namelist,
              int (*filter)(const struct dirent64 *),
              int (*compar)(const struct dirent64 **, const struct dirent64 **)) {
  static scandir64_t real;
  if (!real) real = (scandir64_t)dlsym(RTLD_NEXT, "scandir64");
  char buf[4096];
  return real(rewrite(dirp, buf, sizeof buf), namelist, filter, compar);
}

/* getmntent()/getmntent_r() take a FILE*, so only setmntent()'s path is
 * rewritten. */
typedef FILE *(*setmntent_t)(const char *, const char *);
FILE *setmntent(const char *filename, const char *type) {
  static setmntent_t real;
  if (!real) real = (setmntent_t)dlsym(RTLD_NEXT, "setmntent");
  char buf[4096];
  return real(rewrite(filename, buf, sizeof buf), type);
}

/* ---- access ---------------------------------------------------------- */

typedef int (*access_t)(const char *, int);
int access(const char *pathname, int mode) {
  static access_t real;
  if (!real) real = (access_t)dlsym(RTLD_NEXT, "access");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*faccessat_t)(int, const char *, int, int);
int faccessat(int dirfd, const char *pathname, int mode, int flags) {
  static faccessat_t real;
  if (!real) real = (faccessat_t)dlsym(RTLD_NEXT, "faccessat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode, flags);
}

int faccessat2(int dirfd, const char *pathname, int mode, int flags) {
  static faccessat_t real;
  if (!real) real = (faccessat_t)dlsym(RTLD_NEXT, "faccessat2");
  if (!real) return faccessat(dirfd, pathname, mode, flags);
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode, flags);
}

typedef int (*eaccess_t)(const char *, int);
int eaccess(const char *pathname, int mode) {
  static eaccess_t real;
  if (!real) real = (eaccess_t)dlsym(RTLD_NEXT, "eaccess");
  if (!real) real = (eaccess_t)dlsym(RTLD_NEXT, "euidaccess");
  if (!real) return -1;
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

int euidaccess(const char *pathname, int mode) {
  return eaccess(pathname, mode);
}

/* coreutils mkdir -p verifies an existing component with chdir(), not
 * stat() -- found by strace: mkdirat("$INSTDIR/var") = EEXIST, then
 * chdir("/var") = ENOENT (the real /var does not exist on Android), which
 * coreutils reads as "not a directory" and aborts. Without this, every
 * `mkdir -p` on a path whose parent already exists fails. */
typedef int (*chdir_t)(const char *);
int chdir(const char *pathname) {
  static chdir_t real;
  if (!real) real = (chdir_t)dlsym(RTLD_NEXT, "chdir");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf));
}

/* ---- namespace-ish operations (mkdir/rm/ln/mv/...) ------------------- */

typedef int (*mkdir_t)(const char *, mode_t);
int mkdir(const char *pathname, mode_t mode) {
  static mkdir_t real;
  if (!real) real = (mkdir_t)dlsym(RTLD_NEXT, "mkdir");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*mkdirat_t)(int, const char *, mode_t);
int mkdirat(int dirfd, const char *pathname, mode_t mode) {
  static mkdirat_t real;
  if (!real) real = (mkdirat_t)dlsym(RTLD_NEXT, "mkdirat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*unlink_t)(const char *);
int unlink(const char *pathname) {
  static unlink_t real;
  if (!real) real = (unlink_t)dlsym(RTLD_NEXT, "unlink");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf));
}

typedef int (*unlinkat_t)(int, const char *, int);
int unlinkat(int dirfd, const char *pathname, int flags) {
  static unlinkat_t real;
  if (!real) real = (unlinkat_t)dlsym(RTLD_NEXT, "unlinkat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags);
}

typedef int (*rmdir_t)(const char *);
int rmdir(const char *pathname) {
  static rmdir_t real;
  if (!real) real = (rmdir_t)dlsym(RTLD_NEXT, "rmdir");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf));
}

int remove(const char *pathname) {
  char buf[4096];
  const char *p = rewrite(pathname, buf, sizeof buf);
  if (unlink(p) == 0) return 0;
  return rmdir(p);
}

typedef int (*symlink_t)(const char *, const char *);
int symlink(const char *target, const char *linkpath) {
  static symlink_t real;
  if (!real) real = (symlink_t)dlsym(RTLD_NEXT, "symlink");
  char buf[4096];
  return real(target, rewrite(linkpath, buf, sizeof buf));
}

typedef int (*symlinkat_t)(const char *, int, const char *);
int symlinkat(const char *target, int newdirfd, const char *linkpath) {
  static symlinkat_t real;
  if (!real) real = (symlinkat_t)dlsym(RTLD_NEXT, "symlinkat");
  char buf[4096];
  return real(target, newdirfd, rewrite(linkpath, buf, sizeof buf));
}

typedef int (*link_t)(const char *, const char *);

/* Android forbids hard links in app data (linkat -> EACCES, both from
 * Bionic and glibc); Debian dpkg's status-old backup uses linkat and dies
 * "error creating new backup file ... Permission denied". Fall back to a
 * content copy when the real link is refused -- semantically fine for the
 * backup and for dpkg's dedup uses. */
static int dn_copy_file(const char *oldp, const char *newp) {
  int in = open(oldp, O_RDONLY | O_CLOEXEC);
  if (in < 0) return -1;
  struct stat st;
  if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) { close(in); errno = EPERM; return -1; }
  int out = open(newp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
  if (out < 0) { close(in); return -1; }
  char buf[65536];
  ssize_t r;
  int ok = 0;
  while ((r = read(in, buf, sizeof buf)) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = write(out, buf + off, (size_t)(r - off));
      if (w <= 0) { close(in); close(out); return -1; }
      off += w;
    }
  }
  if (r == 0) ok = 1;
  close(in);
  close(out);
  return ok ? 0 : -1;
}

static int link_fallback(link_t real, const char *o, const char *n) {
  int r = real(o, n);
  if (r == 0) return 0;
  int e = errno;
  if (e == EACCES || e == EPERM || e == EXDEV || e == ENOSYS || e == EOPNOTSUPP) {
    if (dn_copy_file(o, n) == 0) return 0;
  }
  errno = e;
  return -1;
}

int link(const char *oldpath, const char *newpath) {
  static link_t real;
  if (!real) real = (link_t)dlsym(RTLD_NEXT, "link");
  char b1[4096], b2[4096];
  return link_fallback(real, rewrite(oldpath, b1, sizeof b1),
                       rewrite(newpath, b2, sizeof b2));
}

typedef int (*linkat_t)(int, const char *, int, const char *, int);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
  static linkat_t real;
  if (!real) real = (linkat_t)dlsym(RTLD_NEXT, "linkat");
  char b1[4096], b2[4096];
  const char *o = rewrite(oldpath, b1, sizeof b1);
  const char *n = rewrite(newpath, b2, sizeof b2);
  int r = real(olddirfd, o, newdirfd, n, flags);
  if (r == 0) return 0;
  int e = errno;
  if ((e == EACCES || e == EPERM || e == EXDEV || e == ENOSYS || e == EOPNOTSUPP) &&
      olddirfd == AT_FDCWD && newdirfd == AT_FDCWD) {
    if (dn_copy_file(o, n) == 0) return 0;
  }
  errno = e;
  return -1;
}

typedef int (*rename_t)(const char *, const char *);
int rename(const char *oldpath, const char *newpath) {
  static rename_t real;
  if (!real) real = (rename_t)dlsym(RTLD_NEXT, "rename");
  char b1[4096], b2[4096];
  return real(rewrite(oldpath, b1, sizeof b1), rewrite(newpath, b2, sizeof b2));
}

typedef int (*renameat_t)(int, const char *, int, const char *);
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
  static renameat_t real;
  if (!real) real = (renameat_t)dlsym(RTLD_NEXT, "renameat");
  char b1[4096], b2[4096];
  return real(olddirfd, rewrite(oldpath, b1, sizeof b1), newdirfd,
              rewrite(newpath, b2, sizeof b2));
}

typedef int (*renameat2_t)(int, const char *, int, const char *, unsigned int);
int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags) {
  static renameat2_t real;
  if (!real) real = (renameat2_t)dlsym(RTLD_NEXT, "renameat2");
  if (!real) return renameat(olddirfd, oldpath, newdirfd, newpath);
  char b1[4096], b2[4096];
  return real(olddirfd, rewrite(oldpath, b1, sizeof b1), newdirfd,
              rewrite(newpath, b2, sizeof b2), flags);
}

typedef int (*chmod_t)(const char *, mode_t);
int chmod(const char *pathname, mode_t mode) {
  static chmod_t real;
  if (!real) real = (chmod_t)dlsym(RTLD_NEXT, "chmod");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*fchmodat_t)(int, const char *, mode_t, int);
int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
  static fchmodat_t real;
  if (!real) real = (fchmodat_t)dlsym(RTLD_NEXT, "fchmodat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode, flags);
}

typedef int (*truncate_t)(const char *, off_t);
int truncate(const char *pathname, off_t length) {
  static truncate_t real;
  if (!real) real = (truncate_t)dlsym(RTLD_NEXT, "truncate");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), length);
}

/* The *64 names are distinct symbols a caller can reference even where
 * off_t is already 64-bit; without these the base-symbol override above is
 * bypassed. */
typedef FILE *(*fopen64_t)(const char *, const char *);
FILE *fopen64(const char *pathname, const char *mode) {
  static fopen64_t real;
  if (!real) real = (fopen64_t)dlsym(RTLD_NEXT, "fopen64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef FILE *(*freopen64_t)(const char *, const char *, FILE *);
FILE *freopen64(const char *pathname, const char *mode, FILE *stream) {
  static freopen64_t real;
  if (!real) real = (freopen64_t)dlsym(RTLD_NEXT, "freopen64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode, stream);
}

typedef int (*openat64_t)(int, const char *, int, ...);
int openat64(int dirfd, const char *pathname, int flags, ...) {
  static openat64_t real;
  if (!real) real = (openat64_t)dlsym(RTLD_NEXT, "openat64");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags, mode);
}

typedef int (*fstatat64_t)(int, const char *, struct stat64 *, int);
int fstatat64(int dirfd, const char *pathname, struct stat64 *st, int flags) {
  static fstatat64_t real;
  if (!real) real = (fstatat64_t)dlsym(RTLD_NEXT, "fstatat64");
  char buf[4096];
  int r = real(dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*truncate64_t)(const char *, off64_t);
int truncate64(const char *pathname, off64_t length) {
  static truncate64_t real;
  if (!real) real = (truncate64_t)dlsym(RTLD_NEXT, "truncate64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), length);
}

typedef int (*utimensat_t)(int, const char *, const struct timespec *, int);
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags) {
  static utimensat_t real;
  if (!real) real = (utimensat_t)dlsym(RTLD_NEXT, "utimensat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), times, flags);
}

typedef int (*utimes_t)(const char *, const struct timeval *);
int utimes(const char *pathname, const struct timeval times[2]) {
  static utimes_t real;
  if (!real) real = (utimes_t)dlsym(RTLD_NEXT, "utimes");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), times);
}

typedef int (*lutimes_t)(const char *, const struct timeval *);
int lutimes(const char *pathname, const struct timeval times[2]) {
  static lutimes_t real;
  if (!real) real = (lutimes_t)dlsym(RTLD_NEXT, "lutimes");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), times);
}

typedef ssize_t (*readlink_t)(const char *, char *, size_t);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
  static readlink_t real;
  if (!real) real = (readlink_t)dlsym(RTLD_NEXT, "readlink");
  char b[4096];
  return real(rewrite(pathname, b, sizeof b), buf, bufsiz);
}

typedef ssize_t (*readlinkat_t)(int, const char *, char *, size_t);
ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
  static readlinkat_t real;
  if (!real) real = (readlinkat_t)dlsym(RTLD_NEXT, "readlinkat");
  char b[4096];
  return real(dirfd, rewrite(pathname, b, sizeof b), buf, bufsiz);
}

/* ---- exec ------------------------------------------------------------ */

typedef int (*execve_t)(const char *, char *const[], char *const[]);

static int do_exec(execve_t real, const char *rp, char *const argv[],
                   char *const envp[]) {
  char interp[256], sarg[256];
  char ibuf[4096];
  if (g_debug) fprintf(stderr, "[path-redirect] do_exec: rp=%s\n", rp ? rp : "(null)");
  if (target_is_script(rp, interp, sizeof interp, sarg, sizeof sarg)) {
    if (g_debug) fprintf(stderr, "[path-redirect] do_exec: script branch, interp=%s\n", interp);
    const char *iw = map_shebang_interp(interp, ibuf, sizeof ibuf);
    if (iw && access(iw, X_OK) == 0) {
      static char *na[1024];
      int ac = 0;
      while (argv && argv[ac]) ac++;
      int idx = 0;
      na[idx++] = (char *)iw;
      if (sarg[0]) na[idx++] = sarg;
      na[idx++] = (char *)rp;
      for (int i = 1; i < ac && idx < 1022; i++) na[idx++] = argv[i];
      na[idx] = NULL;
      /* The interpreter is itself almost always a shell wrapper script
       * (#!/system/bin/sh) which sets LD_PRELOAD for the real glibc
       * interpreter it execs -- passing ours through here would hand a
       * glibc .so to Bionic's /system/bin/sh and abort it. */
      return real(iw, na, bionic_env(envp));
    }
    return real(rp, argv, bionic_env(envp));
  }
  if (target_is_glibc(rp)) {
    if (g_debug) fprintf(stderr, "[path-redirect] do_exec: glibc branch (LD_PRELOAD kept), rp=%s\n", rp);
    return real(rp, argv, envp);
  }
  if (g_debug) fprintf(stderr, "[path-redirect] do_exec: bionic branch (LD_PRELOAD stripped), rp=%s\n", rp);
  return real(rp, argv, bionic_env(envp));
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
  static execve_t real;
  if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execve");
  char buf[4096];
  if (g_debug) fprintf(stderr, "[path-redirect] execve() called: pathname=%s\n", pathname ? pathname : "(null)");
  return do_exec(real, rewrite(pathname, buf, sizeof buf), argv, envp);
}

int execv(const char *pathname, char *const argv[]) {
  static execve_t real;
  if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execv");
  char buf[4096];
  return do_exec(real, rewrite(pathname, buf, sizeof buf), argv, environ);
}

/* glibc's execvp/execvpe do their own PATH walk and then call __execve
 * *internally*, bypassing the dynamic symbol table -- so exporting execve()
 * alone never sees them. Perl (and plenty else) execs a script with
 * execvp(), which is how debconf's frontend re-runs a package's config
 * script: uncaught, the kernel resolved that script's shebang and handed
 * perl's glibc LD_PRELOAD straight to the Bionic interpreter. Reimplement
 * the PATH walk here and funnel through do_exec/execve. */
static int path_search_exec(const char *file, char *const argv[], char *const envp[]) {
  if (strchr(file, '/')) return execve(file, argv, envp);
  const char *path = getenv("PATH");
  if (g_debug) fprintf(stderr, "[path-redirect] path_search_exec: file=%s PATH=%s\n", file, path ? path : "(null)");
  if (!path || !*path) path = "/bin:/usr/bin";
  char buf[4096];
  const char *p = path;
  for (;;) {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len && len < sizeof buf - 2) {
      memcpy(buf, p, len);
      buf[len] = '/';
      size_t fl = strlen(file);
      if (len + 1 + fl < sizeof buf) {
        memcpy(buf + len + 1, file, fl + 1);
        if (access(buf, X_OK) == 0) {
          if (g_debug) fprintf(stderr, "[path-redirect] path_search_exec: found %s\n", buf);
          return execve(buf, argv, envp);
        }
      }
    }
    if (!end) break;
    p = end + 1;
  }
  if (access(file, X_OK) == 0) return execve(file, argv, envp);
  if (g_debug) fprintf(stderr, "[path-redirect] path_search_exec: ENOENT for %s\n", file);
  errno = ENOENT;
  return -1;
}

int execvp(const char *file, char *const argv[]) {
  if (g_debug) fprintf(stderr, "[path-redirect] execvp() called: file=%s\n", file ? file : "(null)");
  return path_search_exec(file, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
  return path_search_exec(file, argv, envp);
}

static int build_exec_args(const char *first, va_list ap, char **argv, int max) {
  int n = 0;
  if (first) argv[n++] = (char *)first;
  while (n < max - 1) {
    char *a = va_arg(ap, char *);
    if (!a) break;
    argv[n++] = a;
  }
  argv[n] = NULL;
  return n;
}

int execl(const char *pathname, const char *arg, ...) {
  char *argv[256];
  va_list ap; va_start(ap, arg);
  build_exec_args(arg, ap, argv, 256);
  va_end(ap);
  return execve(pathname, argv, environ);
}

int execlp(const char *file, const char *arg, ...) {
  char *argv[256];
  va_list ap; va_start(ap, arg);
  build_exec_args(arg, ap, argv, 256);
  va_end(ap);
  return execvp(file, argv);
}

int execle(const char *pathname, const char *arg, ...) {
  char *argv[256];
  va_list ap; va_start(ap, arg);
  build_exec_args(arg, ap, argv, 256);
  char *const *envp = va_arg(ap, char *const *);
  va_end(ap);
  return execve(pathname, argv, envp);
}

typedef int (*execveat_t)(int, const char *, char *const[], char *const[], int);
int execveat(int dirfd, const char *pathname, char *const argv[],
             char *const envp[], int flags) {
  static execveat_t real;
  if (!real) real = (execveat_t)dlsym(RTLD_NEXT, "execveat");
  char buf[4096];
  const char *rp = rewrite(pathname, buf, sizeof buf);
  if (target_is_glibc(rp)) return real(dirfd, rp, argv, envp, flags);
  return real(dirfd, rp, argv, bionic_env(envp), flags);
}

/* ---- system()/popen(): the shell glibc uses by default ---------------- */

/* glibc's system() and popen() exec `/bin/sh` through libc-internal calls
 * that never reach the interposed execve/posix_spawn symbols, so the shim
 * cannot redirect them. On Android `/bin/sh` is Bionic toybox, and the
 * environment glibc hands it carries the shim's glibc preload, so the
 * Bionic linker aborts ("CANNOT LINK EXECUTABLE ... libc.so.6") -- exactly
 * why apt's hardcoded `Args[0]="/bin/sh"` hooks fail with a real Debian
 * apt/dpkg in the prefix (found 2026-10-04). Interpose both and run the
 * command through the prefix's own dash instead. */
static const char *prefix_shell(void) {
  if (!g_init) dn_init();
  if (!g_root) return NULL;
  static char sh[4096];
  snprintf(sh, sizeof sh, "%s/usr/bin/dash", g_root);
  if (access(sh, X_OK) == 0) return sh;
  snprintf(sh, sizeof sh, "%s/bin/sh", g_root);
  return sh;
}

int system(const char *command) {
  if (!command) return 1;   /* "is a shell available?" -- yes, the prefix's */
  const char *sh = prefix_shell();
  if (!sh) {
    static int (*real)(const char *);
    if (!real) real = (int (*)(const char *))dlsym(RTLD_NEXT, "system");
    return real ? real(command) : -1;
  }
  struct sigaction ign, sa_int, sa_quit;
  memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigaction(SIGINT, &ign, &sa_int);
  sigaction(SIGQUIT, &ign, &sa_quit);
  pid_t pid = fork();
  if (pid == 0) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGINT, &dfl, NULL);
    sigaction(SIGQUIT, &dfl, NULL);
    execl(sh, "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  if (pid < 0) {
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGQUIT, &sa_quit, NULL);
    return -1;
  }
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) { /* retry */ }
  sigaction(SIGINT, &sa_int, NULL);
  sigaction(SIGQUIT, &sa_quit, NULL);
  return status;
}

/* popen/pclose: same reasoning; track the child pid per FILE* so pclose()
 * can wait on it. A small fixed table is enough (popen() is used a handful
 * of times at once, never in the thousands). */
struct pclose_ent { FILE *f; pid_t pid; };
static struct pclose_ent pclose_reg[64];
static int pclose_n;

FILE *popen(const char *command, const char *type) {
  const char *sh = prefix_shell();
  if (!sh) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "popen");
    return real ? real(command, type) : NULL;
  }
  int reading = (type && type[0] == 'r');
  int pfd[2];
  if (pipe(pfd) != 0) return NULL;
  pid_t pid = fork();
  if (pid < 0) { close(pfd[0]); close(pfd[1]); return NULL; }
  if (pid == 0) {
    if (reading) dup2(pfd[1], STDOUT_FILENO);
    else         dup2(pfd[0], STDIN_FILENO);
    close(pfd[0]);
    close(pfd[1]);
    execl(sh, "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  FILE *f;
  if (reading) { close(pfd[1]); f = fdopen(pfd[0], "r"); }
  else         { close(pfd[0]); f = fdopen(pfd[1], "w"); }
  if (!f) {
    if (reading) close(pfd[0]); else close(pfd[1]);
    return NULL;
  }
  for (int i = 0; i < pclose_n; i++) {
    if (!pclose_reg[i].f) { pclose_reg[i].f = f; pclose_reg[i].pid = pid; return f; }
  }
  if (pclose_n < 64) {
    pclose_reg[pclose_n].f = f;
    pclose_reg[pclose_n].pid = pid;
    pclose_n++;
    return f;
  }
  fclose(f);   /* table exhausted (should not happen) */
  return NULL;
}

int pclose(FILE *stream) {
  pid_t pid = -1;
  for (int i = 0; i < pclose_n; i++) {
    if (pclose_reg[i].f == stream) { pid = pclose_reg[i].pid; pclose_reg[i].f = NULL; break; }
  }
  if (pid < 0) {
    static int (*real)(FILE *);
    if (!real) real = (int (*)(FILE *))dlsym(RTLD_NEXT, "pclose");
    return real ? real(stream) : -1;
  }
  fclose(stream);
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) { /* retry */ }
  return status;
}

/* ---- dlopen / dlmopen ------------------------------------------------- */

/* glibc's loader opens an object internally rather than through the
 * interposable PLT, but it does so from these entry points, so rewriting
 * the name here covers the common `dlopen("/usr/lib/...")` case. A NULL
 * name (re-open the main program) is passed through untouched. */
typedef void *(*dlopen_t)(const char *, int);
void *dlopen(const char *filename, int flags) {
  static dlopen_t real;
  if (!real) real = (dlopen_t)dlsym(RTLD_NEXT, "dlopen");
  if (!filename) return real(NULL, flags);
  char buf[4096];
  return real(rewrite(filename, buf, sizeof buf), flags);
}

typedef void *(*dlmopen_t)(Lmid_t, const char *, int);
void *dlmopen(Lmid_t nsid, const char *filename, int flags) {
  static dlmopen_t real;
  if (!real) real = (dlmopen_t)dlsym(RTLD_NEXT, "dlmopen");
  if (!filename) return real(nsid, NULL, flags);
  char buf[4096];
  return real(nsid, rewrite(filename, buf, sizeof buf), flags);
}

/* ---- AF_UNIX socket paths -------------------------------------------- */

/* A filesystem sun_path is just another absolute path, but sun_path is only
 * 108 bytes: a rewritten path that would not fit is left untouched rather
 * than truncated (a wrong socket is worse than an honest ENOENT). Abstract
 * sockets (first byte '\0') are ignored -- rewrite() only acts on a leading
 * '/'. The caller's addr may be shorter than struct sockaddr_un, so copy at
 * most addrlen bytes. */
static int rewrite_sockaddr(const struct sockaddr *addr, socklen_t addrlen,
                            struct sockaddr_un *out, socklen_t *outlen) {
  if (!addr || addr->sa_family != AF_UNIX) return 0;
  if (addrlen <= (socklen_t)offsetof(struct sockaddr_un, sun_path)) return 0;
  memset(out, 0, sizeof *out);
  memcpy(out, addr, addrlen < (socklen_t)sizeof *out ? addrlen : (socklen_t)sizeof *out);
  if (out->sun_path[0] != '/') return 0;
  char buf[4096];
  const char *rp = rewrite(out->sun_path, buf, sizeof buf);
  if (rp == out->sun_path) return 0;
  size_t rl = strlen(rp);
  if (rl >= sizeof out->sun_path) return 0;
  memcpy(out->sun_path, rp, rl + 1);
  *outlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + rl + 1);
  return 1;
}

typedef int (*bind_t)(int, const struct sockaddr *, socklen_t);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  static bind_t real;
  if (!real) real = (bind_t)dlsym(RTLD_NEXT, "bind");
  struct sockaddr_un un;
  socklen_t unlen;
  if (rewrite_sockaddr(addr, addrlen, &un, &unlen))
    return real(sockfd, (const struct sockaddr *)&un, unlen);
  return real(sockfd, addr, addrlen);
}

typedef int (*connect_t)(int, const struct sockaddr *, socklen_t);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  static connect_t real;
  if (!real) real = (connect_t)dlsym(RTLD_NEXT, "connect");
  struct sockaddr_un un;
  socklen_t unlen;
  if (rewrite_sockaddr(addr, addrlen, &un, &unlen))
    return real(sockfd, (const struct sockaddr *)&un, unlen);
  return real(sockfd, addr, addrlen);
}

/* ---- file creation / stdio ----------------------------------------- */

typedef int (*creat_t)(const char *, mode_t);
int creat(const char *pathname, mode_t mode) {
  static creat_t real;
  if (!real) real = (creat_t)dlsym(RTLD_NEXT, "creat");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*creat64_t)(const char *, mode_t);
int creat64(const char *pathname, mode_t mode) {
  static creat64_t real;
  if (!real) real = (creat64_t)dlsym(RTLD_NEXT, "creat64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef FILE *(*freopen_t)(const char *, const char *, FILE *);
FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
  static freopen_t real;
  if (!real) real = (freopen_t)dlsym(RTLD_NEXT, "freopen");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode, stream);
}

/* ---- ownership / times --------------------------------------------- */

typedef int (*chown_t)(const char *, uid_t, gid_t);
int chown(const char *pathname, uid_t owner, gid_t group) {
  static chown_t real;
  if (!real) real = (chown_t)dlsym(RTLD_NEXT, "chown");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), owner, group);
  FAKE_OK(r);
}

typedef int (*lchown_t)(const char *, uid_t, gid_t);
int lchown(const char *pathname, uid_t owner, gid_t group) {
  static lchown_t real;
  if (!real) real = (lchown_t)dlsym(RTLD_NEXT, "lchown");
  char buf[4096];
  int r = real(rewrite(pathname, buf, sizeof buf), owner, group);
  FAKE_OK(r);
}

typedef int (*fchownat_t)(int, const char *, uid_t, gid_t, int);
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group,
             int flags) {
  static fchownat_t real;
  if (!real) real = (fchownat_t)dlsym(RTLD_NEXT, "fchownat");
  char buf[4096];
  int r = real(dirfd, rewrite(pathname, buf, sizeof buf), owner, group,
               flags);
  FAKE_OK(r);
}

/* fd-based stat and chown: no path to rewrite, only the fake owner. */
typedef int (*fstat_t)(int, struct stat *);
int fstat(int fd, struct stat *st) {
  static fstat_t real;
  if (!real) real = (fstat_t)dlsym(RTLD_NEXT, "fstat");
  int r = real(fd, st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*fstat64_t)(int, struct stat64 *);
int fstat64(int fd, struct stat64 *st) {
  static fstat64_t real;
  if (!real) real = (fstat64_t)dlsym(RTLD_NEXT, "fstat64");
  int r = real(fd, st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*fxstat_t)(int, int, struct stat *);
int __fxstat(int ver, int fd, struct stat *st) {
  static fxstat_t real;
  if (!real) real = (fxstat_t)dlsym(RTLD_NEXT, "__fxstat");
  if (!real) return -1;
  int r = real(ver, fd, st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*fxstat64_t)(int, int, struct stat64 *);
int __fxstat64(int ver, int fd, struct stat64 *st) {
  static fxstat64_t real;
  if (!real) real = (fxstat64_t)dlsym(RTLD_NEXT, "__fxstat64");
  if (!real) return -1;
  int r = real(ver, fd, st);
  if (r == 0) FAKE_OWNER(st);
  return r;
}

typedef int (*fchown_t)(int, uid_t, gid_t);
int fchown(int fd, uid_t owner, gid_t group) {
  static fchown_t real;
  if (!real) real = (fchown_t)dlsym(RTLD_NEXT, "fchown");
  int r = real(fd, owner, group);
  FAKE_OK(r);
}

/* ---- identity (fake root, see dn_init) ------------------------------ */

#define REAL_ID(name, type)                                          \
  static type (*real)(void);                                         \
  if (!real) real = (type (*)(void))dlsym(RTLD_NEXT, name);

uid_t getuid(void)  { REAL_ID("getuid", uid_t)  return g_fakeroot ? 0 : real(); }
uid_t geteuid(void) { REAL_ID("geteuid", uid_t) return g_fakeroot ? 0 : real(); }
gid_t getgid(void)  { REAL_ID("getgid", gid_t)  return g_fakeroot ? 0 : real(); }
gid_t getegid(void) { REAL_ID("getegid", gid_t) return g_fakeroot ? 0 : real(); }

typedef int (*getres_t)(unsigned int *, unsigned int *, unsigned int *);
int getresuid(uid_t *r, uid_t *e, uid_t *s) {
  static getres_t real;
  if (!real) real = (getres_t)dlsym(RTLD_NEXT, "getresuid");
  if (!g_fakeroot) return real(r, e, s);
  *r = *e = *s = 0;
  return 0;
}
int getresgid(gid_t *r, gid_t *e, gid_t *s) {
  static getres_t real;
  if (!real) real = (getres_t)dlsym(RTLD_NEXT, "getresgid");
  if (!g_fakeroot) return real(r, e, s);
  *r = *e = *s = 0;
  return 0;
}

typedef int (*getgroups_t)(int, gid_t *);
int getgroups(int size, gid_t list[]) {
  static getgroups_t real;
  if (!real) real = (getgroups_t)dlsym(RTLD_NEXT, "getgroups");
  if (!g_fakeroot) return real(size, list);
  if (size == 0) return 1;
  if (size < 0) { errno = EINVAL; return -1; }
  list[0] = 0;
  return 1;
}

/* Dropping or changing identity "succeeds": a daemon that switches to its
 * service user keeps running as the app user. */
#define FAKE_SETID(name, proto, args)                                \
  typedef int (*name##_t) proto;                                     \
  int name proto {                                                   \
    static name##_t real;                                            \
    if (!real) real = (name##_t)dlsym(RTLD_NEXT, #name);             \
    if (g_fakeroot) return 0;                                        \
    return real args;                                                \
  }
FAKE_SETID(setuid, (uid_t u), (u))
FAKE_SETID(setgid, (gid_t g), (g))
FAKE_SETID(seteuid, (uid_t u), (u))
FAKE_SETID(setegid, (gid_t g), (g))
FAKE_SETID(setreuid, (uid_t r, uid_t e), (r, e))
FAKE_SETID(setregid, (gid_t r, gid_t e), (r, e))
FAKE_SETID(setresuid, (uid_t r, uid_t e, uid_t s), (r, e, s))
FAKE_SETID(setresgid, (gid_t r, gid_t e, gid_t s), (r, e, s))
FAKE_SETID(setgroups, (size_t n, const gid_t *l), (n, l))
FAKE_SETID(initgroups, (const char *u, gid_t g), (u, g))

/* setfsuid()/setfsgid() are NOT in the FAKE_SETID set: Android's seccomp
 * filter traps those syscalls with SIGSYS regardless of ours being fake root,
 * so a real() fallthrough would still kill the process ("Bad system call").
 * They are reached by things that only need to *check* file access as the
 * caller's identity -- ncurses/libtinfo's terminfo lookup (so any interactive
 * bash, via readline) and bash's own access checks. We run as one uid, so make
 * them silent no-ops returning the current uid/gid as the "previous" fsuid. */
int setfsuid(uid_t uid) { (void)uid; return (int)getuid(); }
int setfsgid(gid_t gid) { (void)gid; return (int)getgid(); }

typedef int (*utime_t)(const char *, const struct utimbuf *);
int utime(const char *pathname, const struct utimbuf *times) {
  static utime_t real;
  if (!real) real = (utime_t)dlsym(RTLD_NEXT, "utime");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), times);
}

/* ---- extended attributes -------------------------------------------- */

typedef int (*setxattr_t)(const char *, const char *, const void *, size_t,
                           int);
int setxattr(const char *path, const char *name, const void *value,
             size_t size, int flags) {
  static setxattr_t real;
  if (!real) real = (setxattr_t)dlsym(RTLD_NEXT, "setxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name, value, size, flags);
}

typedef int (*lsetxattr_t)(const char *, const char *, const void *, size_t,
                            int);
int lsetxattr(const char *path, const char *name, const void *value,
              size_t size, int flags) {
  static lsetxattr_t real;
  if (!real) real = (lsetxattr_t)dlsym(RTLD_NEXT, "lsetxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name, value, size, flags);
}

typedef ssize_t (*getxattr_t)(const char *, const char *, void *, size_t);
ssize_t getxattr(const char *path, const char *name, void *value,
                 size_t size) {
  static getxattr_t real;
  if (!real) real = (getxattr_t)dlsym(RTLD_NEXT, "getxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name, value, size);
}

typedef ssize_t (*lgetxattr_t)(const char *, const char *, void *, size_t);
ssize_t lgetxattr(const char *path, const char *name, void *value,
                  size_t size) {
  static lgetxattr_t real;
  if (!real) real = (lgetxattr_t)dlsym(RTLD_NEXT, "lgetxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name, value, size);
}

typedef ssize_t (*listxattr_t)(const char *, char *, size_t);
ssize_t listxattr(const char *path, char *list, size_t size) {
  static listxattr_t real;
  if (!real) real = (listxattr_t)dlsym(RTLD_NEXT, "listxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), list, size);
}

typedef ssize_t (*llistxattr_t)(const char *, char *, size_t);
ssize_t llistxattr(const char *path, char *list, size_t size) {
  static llistxattr_t real;
  if (!real) real = (llistxattr_t)dlsym(RTLD_NEXT, "llistxattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), list, size);
}

typedef int (*removexattr_t)(const char *, const char *);
int removexattr(const char *path, const char *name) {
  static removexattr_t real;
  if (!real) real = (removexattr_t)dlsym(RTLD_NEXT, "removexattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name);
}

typedef int (*lremovexattr_t)(const char *, const char *);
int lremovexattr(const char *path, const char *name) {
  static lremovexattr_t real;
  if (!real) real = (lremovexattr_t)dlsym(RTLD_NEXT, "lremovexattr");
  char buf[4096];
  return real(rewrite(path, buf, sizeof buf), name);
}

/* ---- special files --------------------------------------------------- */

typedef int (*mkfifo_t)(const char *, mode_t);
int mkfifo(const char *pathname, mode_t mode) {
  static mkfifo_t real;
  if (!real) real = (mkfifo_t)dlsym(RTLD_NEXT, "mkfifo");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*mkfifoat_t)(int, const char *, mode_t);
int mkfifoat(int dirfd, const char *pathname, mode_t mode) {
  static mkfifoat_t real;
  if (!real) real = (mkfifoat_t)dlsym(RTLD_NEXT, "mkfifoat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*mknod_t)(const char *, mode_t, dev_t);
int mknod(const char *pathname, mode_t mode, dev_t dev) {
  static mknod_t real;
  if (!real) real = (mknod_t)dlsym(RTLD_NEXT, "mknod");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode, dev);
}

typedef int (*mknodat_t)(int, const char *, mode_t, dev_t);
int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev) {
  static mknodat_t real;
  if (!real) real = (mknodat_t)dlsym(RTLD_NEXT, "mknodat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), mode, dev);
}

/* ---- filesystem stats ------------------------------------------------ */

typedef int (*statfs64_t)(const char *, struct statfs64 *);
int statfs64(const char *pathname, struct statfs64 *st) {
  static statfs64_t real;
  if (!real) real = (statfs64_t)dlsym(RTLD_NEXT, "statfs64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef int (*statvfs64_t)(const char *, struct statvfs64 *);
int statvfs64(const char *pathname, struct statvfs64 *st) {
  static statvfs64_t real;
  if (!real) real = (statvfs64_t)dlsym(RTLD_NEXT, "statvfs64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

/* ---- path resolution ------------------------------------------------- */

char *realpath(const char *path, char *resolved) {
  static char *(*real)(const char *, char *);
  if (!real)
    real = (char *(*)(const char *, char *))dlsym(RTLD_NEXT, "realpath");
  char buf[4096];
  const char *rp = rewrite(path, buf, sizeof buf);
  if (rp == path) return real(path, resolved);
  return real(rp, resolved);
}

char *canonicalize_file_name(const char *path) {
  static char *(*real)(const char *);
  if (!real)
    real = (char *(*)(const char *))dlsym(RTLD_NEXT, "canonicalize_file_name");
  char buf[4096];
  const char *rp = rewrite(path, buf, sizeof buf);
  if (rp == path) return real(path);
  return real(rp);
}

/* ---- inotify --------------------------------------------------------- */

typedef int (*inotify_add_watch_t)(int, const char *, uint32_t);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
  static inotify_add_watch_t real;
  if (!real)
    real = (inotify_add_watch_t)dlsym(RTLD_NEXT, "inotify_add_watch");
  char buf[4096];
  return real(fd, rewrite(pathname, buf, sizeof buf), mask);
}

/* ---- AF_UNIX datagram ------------------------------------------------ */

/* sendto may carry an AF_UNIX sun_path in dest_addr; rewrite it the
 * same way bind()/connect() do via rewrite_sockaddr(). */
typedef ssize_t (*sendto_t)(int, const void *, size_t, int,
                              const struct sockaddr *, socklen_t);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
  static sendto_t real;
  if (!real) real = (sendto_t)dlsym(RTLD_NEXT, "sendto");
  struct sockaddr_un un;
  socklen_t unlen;
  if (rewrite_sockaddr(dest_addr, addrlen, &un, &unlen))
    return real(sockfd, buf, len, flags, (const struct sockaddr *)&un,
                unlen);
  return real(sockfd, buf, len, flags, dest_addr, addrlen);
}

/* sendmsg() carries the same AF_UNIX address in msg_name; copy the msghdr
 * because the caller's is const. */
typedef ssize_t (*sendmsg_t)(int, const struct msghdr *, int);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
  static sendmsg_t real;
  if (!real) real = (sendmsg_t)dlsym(RTLD_NEXT, "sendmsg");
  if (msg && msg->msg_name) {
    struct sockaddr_un un;
    socklen_t unlen;
    if (rewrite_sockaddr((const struct sockaddr *)msg->msg_name,
                         msg->msg_namelen, &un, &unlen)) {
      struct msghdr m = *msg;
      m.msg_name = &un;
      m.msg_namelen = unlen;
      return real(sockfd, &m, flags);
    }
  }
  return real(sockfd, msg, flags);
}

/* ---- temp files ------------------------------------------------------ */

/* mkstemp/mkostemp/mkdtemp modify tmpl in place and the caller's
 * template buffer is only strlen(tmpl)+1 long.  After rewriting into
 * a local buf and calling real(buf), we can copy the rewritten tail
 * back into tmpl only if it fits: strlen(buf+g_rootlen)+1 must be
 * <= strlen(tmpl)+1, i.e. the tail after g_rootlen must not exceed
 * the original template's capacity.  Without this check we could
 * overflow the caller's fixed-size template buffer. */

typedef int (*mkstemp_t)(char *);
int mkstemp(char *tmpl) {
  static mkstemp_t real;
  if (!real) real = (mkstemp_t)dlsym(RTLD_NEXT, "mkstemp");
  char buf[4096];
  const char *rp = rewrite(tmpl, buf, sizeof buf);
  if (rp == tmpl) return real(tmpl);
  int r = real(buf);
  if (r >= 0 && g_root && g_rootlen &&
      strlen(buf + g_rootlen) + 1 <= strlen(tmpl) + 1)
    strcpy(tmpl, buf + g_rootlen);
  return r;
}

typedef int (*mkostemp_t)(char *, int);
int mkostemp(char *tmpl, int flags) {
  static mkostemp_t real;
  if (!real) real = (mkostemp_t)dlsym(RTLD_NEXT, "mkostemp");
  char buf[4096];
  const char *rp = rewrite(tmpl, buf, sizeof buf);
  if (rp == tmpl) return real(tmpl, flags);
  int r = real(buf, flags);
  if (r >= 0 && g_root && g_rootlen &&
      strlen(buf + g_rootlen) + 1 <= strlen(tmpl) + 1)
    strcpy(tmpl, buf + g_rootlen);
  return r;
}

typedef int (*mkstemps_t)(char *, int);
int mkstemps(char *tmpl, int suffixlen) {
  static mkstemps_t real;
  if (!real) real = (mkstemps_t)dlsym(RTLD_NEXT, "mkstemps");
  char buf[4096];
  const char *rp = rewrite(tmpl, buf, sizeof buf);
  if (rp == tmpl) return real(tmpl, suffixlen);
  int r = real(buf, suffixlen);
  if (r >= 0 && g_root && g_rootlen &&
      strlen(buf + g_rootlen) + 1 <= strlen(tmpl) + 1)
    strcpy(tmpl, buf + g_rootlen);
  return r;
}

typedef int (*mkostemps_t)(char *, int, int);
int mkostemps(char *tmpl, int suffixlen, int flags) {
  static mkostemps_t real;
  if (!real) real = (mkostemps_t)dlsym(RTLD_NEXT, "mkostemps");
  char buf[4096];
  const char *rp = rewrite(tmpl, buf, sizeof buf);
  if (rp == tmpl) return real(tmpl, suffixlen, flags);
  int r = real(buf, suffixlen, flags);
  if (r >= 0 && g_root && g_rootlen &&
      strlen(buf + g_rootlen) + 1 <= strlen(tmpl) + 1)
    strcpy(tmpl, buf + g_rootlen);
  return r;
}

typedef char *(*mkdtemp_t)(char *);
char *mkdtemp(char *tmpl) {
  static mkdtemp_t real;
  if (!real) real = (mkdtemp_t)dlsym(RTLD_NEXT, "mkdtemp");
  char buf[4096];
  const char *rp = rewrite(tmpl, buf, sizeof buf);
  if (rp == tmpl) return real(tmpl);
  char *r = real(buf);
  if (!r) return NULL;
  if (g_root && g_rootlen &&
      strlen(buf + g_rootlen) + 1 <= strlen(tmpl) + 1)
    strcpy(tmpl, buf + g_rootlen);
  return tmpl;
}

/* ---- process spawn --------------------------------------------------- */

/* glibc's posix_spawn uses clone+exec internally, bypassing the
 * interposed execve().  We must therefore rewrite the path and
 * adjust the environment ourselves. */
typedef int (*posix_spawn_t)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const [], char *const []);
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
  static posix_spawn_t real;
  if (!real)
    real = (posix_spawn_t)dlsym(RTLD_NEXT, "posix_spawn");
  char buf[4096];
  const char *rp = rewrite(path, buf, sizeof buf);
  /* Same three-way branch as do_exec() (execve's dispatcher) -- found
   * missing here during the 2026-09-30 runtime-component audit: this
   * used to only classify glibc-vs-Bionic and skip the script/shebang
   * branch entirely, so a script spawned via posix_spawn (glibc's own
   * system()/popen() can use it internally) never got its shebang
   * interpreter remapped to the prefix's loader/dn-shell the way execve's targets do,
   * and -- since a plain-text script fails target_is_glibc()'s ELF-magic
   * check -- was always treated as a Bionic target regardless of what it
   * actually needed. */
  char interp[256], sarg[256];
  if (target_is_script(rp, interp, sizeof interp, sarg, sizeof sarg)) {
    char ibuf[4096];
    const char *iw = map_shebang_interp(interp, ibuf, sizeof ibuf);
    if (iw && access(iw, X_OK) == 0) {
      static char *na[1024];
      int ac = 0;
      while (argv && argv[ac]) ac++;
      int idx = 0;
      na[idx++] = (char *)iw;
      if (sarg[0]) na[idx++] = sarg;
      na[idx++] = (char *)rp;
      for (int i = 1; i < ac && idx < 1022; i++) na[idx++] = argv[i];
      na[idx] = NULL;
      return real(pid, iw, fa, attr, na, bionic_env(envp));
    }
    return real(pid, rp, fa, attr, argv, bionic_env(envp));
  }
  if (target_is_glibc(rp))
    return real(pid, rp, fa, attr, argv, envp);
  return real(pid, rp, fa, attr, argv, bionic_env(envp));
}

/* posix_spawnp: glibc's internal PATH walk bypasses our execve
 * interposition, so we walk $PATH ourselves and delegate to
 * posix_spawn.  Do not call the real posix_spawnp. */
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
  if (strchr(file, '/'))
    return posix_spawn(pid, file, fa, attr, argv, envp);
  const char *path = getenv("PATH");
  if (!path || !*path) path = "/bin:/usr/bin";
  char buf[4096];
  const char *p = path;
  for (;;) {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len && len < sizeof buf - 2) {
      memcpy(buf, p, len);
      buf[len] = '/';
      size_t fl = strlen(file);
      if (len + 1 + fl < sizeof buf) {
        memcpy(buf + len + 1, file, fl + 1);
        if (access(buf, X_OK) == 0)
          return posix_spawn(pid, buf, fa, attr, argv, envp);
      }
    }
    if (!end) break;
    p = end + 1;
  }
  if (access(file, X_OK) == 0)
    return posix_spawn(pid, file, fa, attr, argv, envp);
  errno = ENOENT;
  return -1;
}
