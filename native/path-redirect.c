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

/* Cached once, at load. rewrite() is on the hot path of every intercepted
 * open/stat/exec call, so it must not call getenv()/strlen() per call or
 * snprintf() to build the result -- a plain branch + memcpy is enough and
 * measurably faster (see docs/findings.md). The env is
 * fixed before exec by the launchers, so caching at construction is safe. */
static const char *g_root;
static size_t g_rootlen;
static int g_debug;
static const char *g_bionic_preload;
static int g_init;

static void dn_init(void) {
  g_root = getenv("DN_INSTDIR");
  g_rootlen = g_root ? strlen(g_root) : 0;
  g_debug = getenv("DN_REDIRECT_DEBUG") != NULL;
  g_bionic_preload = getenv("DN_BIONIC_PRELOAD");
  g_init = 1;
}
__attribute__((constructor)) static void dn_ctor(void) { dn_init(); }

static const char *rewrite(const char *path, char *buf, size_t bufsz) {
  if (!g_init) dn_init();
  if (!g_root || !path || path[0] != '/') return path;

  /* Only /usr, /etc, /var, /opt qualify; dispatch on the second byte so a
   * non-matching path costs one compare instead of four strncmp()s. */
  const char *pre;
  switch (path[1]) {
    case 'u': pre = "/usr"; break;
    case 'e': pre = "/etc"; break;
    case 'v': pre = "/var"; break;
    case 'o': pre = "/opt"; break;
    default: return path;
  }
  if (strncmp(path, pre, 4) != 0) return path;
  if (path[4] != '/' && path[4] != '\0') return path;

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
static char **bionic_env(char **envp) {
  if (!envp) return envp;
  if (!g_init) dn_init();
  const char *b = g_bionic_preload;
  int want = (b && *b);
  static char entry[8192];
  if (want) snprintf(entry, sizeof entry, "LD_PRELOAD=%s", b);
  static char *out[2048];
  int n = 0, saw = 0;
  for (char **e = envp; *e && n < 2045; e++) {
    if (!strncmp(*e, "LD_PRELOAD=", 11)) {
      saw = 1;
      if (want) out[n++] = entry;
      continue;
    }
    out[n++] = *e;
  }
  if (want && !saw && n < 2045) out[n++] = entry;
  out[n] = NULL;
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
    if (!strcmp(in, "/bin/sh") || !strcmp(in, "/bin/dash") ||
        !strcmp(in, "/bin/bash") || !strcmp(in, "/usr/bin/sh") ||
        !strcmp(in, "/usr/bin/dash") || !strcmp(in, "/usr/bin/bash")) {
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
  return real(dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
}

typedef int (*fxstatat_t)(int, int, const char *, struct stat *, int);
int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *st, int flags) {
  static fxstatat_t real;
  if (!real) real = (fxstatat_t)dlsym(RTLD_NEXT, "__fxstatat");
  if (!real) return -1;
  char buf[4096];
  return real(ver, dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
}

typedef int (*stat_t)(const char *, struct stat *);
int stat(const char *pathname, struct stat *st) {
  static stat_t real;
  if (!real) real = (stat_t)dlsym(RTLD_NEXT, "stat");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef int (*stat64_t)(const char *, struct stat64 *);
int stat64(const char *pathname, struct stat64 *st) {
  static stat64_t real;
  if (!real) real = (stat64_t)dlsym(RTLD_NEXT, "stat64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

int lstat64(const char *pathname, struct stat64 *st) {
  static stat64_t real;
  if (!real) real = (stat64_t)dlsym(RTLD_NEXT, "lstat64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef int (*lstat_t)(const char *, struct stat *);
int lstat(const char *pathname, struct stat *st) {
  static lstat_t real;
  if (!real) real = (lstat_t)dlsym(RTLD_NEXT, "lstat");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef int (*statx_t)(int, const char *, int, unsigned int, struct statx *);
int statx(int dirfd, const char *pathname, int flags, unsigned int mask,
          struct statx *stx) {
  static statx_t real;
  if (!real) real = (statx_t)dlsym(RTLD_NEXT, "statx");
  if (!real) return -1;
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags, mask, stx);
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

int link(const char *oldpath, const char *newpath) {
  static symlink_t real;
  if (!real) real = (symlink_t)dlsym(RTLD_NEXT, "link");
  char b1[4096], b2[4096];
  return real(rewrite(oldpath, b1, sizeof b1), rewrite(newpath, b2, sizeof b2));
}

typedef int (*linkat_t)(int, const char *, int, const char *, int);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
  static linkat_t real;
  if (!real) real = (linkat_t)dlsym(RTLD_NEXT, "linkat");
  char b1[4096], b2[4096];
  return real(olddirfd, rewrite(oldpath, b1, sizeof b1), newdirfd,
              rewrite(newpath, b2, sizeof b2), flags);
}

int rename(const char *oldpath, const char *newpath) {
  static symlink_t real;
  if (!real) real = (symlink_t)dlsym(RTLD_NEXT, "rename");
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
  return real(dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
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
  if (target_is_script(rp, interp, sizeof interp, sarg, sizeof sarg)) {
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
  if (target_is_glibc(rp)) return real(rp, argv, envp);
  return real(rp, argv, bionic_env(envp));
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
  static execve_t real;
  if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execve");
  char buf[4096];
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
        if (access(buf, X_OK) == 0) return execve(buf, argv, envp);
      }
    }
    if (!end) break;
    p = end + 1;
  }
  if (access(file, X_OK) == 0) return execve(file, argv, envp);
  errno = ENOENT;
  return -1;
}

int execvp(const char *file, char *const argv[]) {
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
