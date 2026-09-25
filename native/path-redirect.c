/* "Manual overlay" — the middleman between a package's hardcoded absolute
 * paths and where those files actually live on Termux, without any of
 * sudo-less's kernel mechanisms (mount namespace + overlayfs), because
 * neither is available on this device: unshare(CLONE_NEWUSER) fails with
 * EINVAL (kernel built without user namespace support, not merely an
 * SELinux denial), plain unshare(CLONE_NEWNS) fails with EPERM (needs
 * CAP_SYS_ADMIN or a user namespace, neither obtainable), and FUSE is not
 * usable either (/dev/fuse: Permission denied, no fusermount). See
 * docs/design-manual-overlay.md.
 *
 * This is an LD_PRELOAD shared library: it intercepts the libc calls a
 * dynamically-linked glibc binary uses to open/stat/exec a file and
 * rewrites any path under one of four directories (/usr, /etc, /var,
 * /opt — the same ones sudo-less's "view" overlaid) to the same path
 * under DN_INSTDIR, before handing it to the real libc function. No
 * kernel privilege is needed — this works entirely at userspace symbol
 * interposition.
 *
 * Verified against two real gaps:
 * - figlet-figlet (figlet_2.2.5-3+b2, Debian's own binary): fstatat() on
 *   the hardcoded /usr/share/figlet/standard.flf, no env var or CLI flag
 *   able to redirect it — exactly the case sudo-less's docs (view.md)
 *   cite as needing their view. Prints the ASCII banner correctly with
 *   this shim + DN_INSTDIR set.
 * - a real Debian glibc `dash` (installed via this project's own apt
 *   pipeline, ELF-patched with `grun --configure` like any other glibc
 *   binary), running `. /etc/foo.conf`: dash imports open64/stat64/
 *   lstat64 (LFS variants — a modern glibc target compiles plain open()/
 *   stat() source into these by default), not the plain names first
 *   tried; missed on the first attempt, found via `readelf --dyn-syms`,
 *   fixed by intercepting both. This is the mechanism maintainer scripts
 *   actually use once their shebang points at this dash instead of the
 *   real /bin/sh (docs/design-manual-overlay.md) — no dpkg patch needed,
 *   no Bionic shim needed: dpkg just execve()s the script file directly
 *   and lets the kernel resolve its shebang, so rewriting the shebang
 *   line itself (already done by patch-maintainer-scripts.sh, which
 *   already edits this same file's text) is enough.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <unistd.h>

static const char *PREFIXES[] = {"/usr", "/etc", "/var", "/opt", NULL};

static const char *rewrite(const char *path, char *buf, size_t bufsz) {
  const char *root = getenv("DN_INSTDIR");
  if (!root || !path) return path;
  for (int i = 0; PREFIXES[i]; i++) {
    size_t plen = strlen(PREFIXES[i]);
    if (strncmp(path, PREFIXES[i], plen) == 0 &&
        (path[plen] == '/' || path[plen] == '\0')) {
      snprintf(buf, bufsz, "%s%s", root, path);
      if (getenv("DN_REDIRECT_DEBUG"))
        fprintf(stderr, "[path-redirect] %s -> %s\n", path, buf);
      return buf;
    }
  }
  return path;
}

/* open64/stat64/... too: a modern glibc target compiles plain open()/
 * stat() source calls into these LFS ("large file support") variants by
 * default (_FILE_OFFSET_BITS=64) -- confirmed the hard way against a
 * real Debian dash binary, whose dynamic symbol table imports open64/
 * stat64/lstat64/fstat64, not the plain names this file used to only
 * intercept. */
typedef int (*open64_t)(const char *, int, ...);
int open64(const char *pathname, int flags, ...) {
  static open64_t real = NULL;
  if (!real) real = (open64_t)dlsym(RTLD_NEXT, "open64");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(rewrite(pathname, buf, sizeof buf), flags, mode);
}

typedef int (*openat_t)(int, const char *, int, ...);
int openat(int dirfd, const char *pathname, int flags, ...) {
  static openat_t real = NULL;
  if (!real) real = (openat_t)dlsym(RTLD_NEXT, "openat");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(dirfd, rewrite(pathname, buf, sizeof buf), flags, mode);
}

typedef int (*open_t)(const char *, int, ...);
int open(const char *pathname, int flags, ...) {
  static open_t real = NULL;
  if (!real) real = (open_t)dlsym(RTLD_NEXT, "open");
  char buf[4096];
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
  return real(rewrite(pathname, buf, sizeof buf), flags, mode);
}

/* The one actually hit by figlet (confirmed via strace: raw syscall
 * newfstatat). Modern glibc's stat()/lstat() go through this, not the
 * older versioned __fxstatat — both are intercepted since which symbol a
 * given glibc/binary combination resolves to isn't safe to assume. */
typedef int (*fstatat_t)(int, const char *, struct stat *, int);
int fstatat(int dirfd, const char *pathname, struct stat *st, int flags) {
  static fstatat_t real = NULL;
  if (!real) real = (fstatat_t)dlsym(RTLD_NEXT, "fstatat");
  char buf[4096];
  return real(dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
}

typedef int (*fxstatat_t)(int, int, const char *, struct stat *, int);
int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *st, int flags) {
  static fxstatat_t real = NULL;
  if (!real) real = (fxstatat_t)dlsym(RTLD_NEXT, "__fxstatat");
  char buf[4096];
  return real(ver, dirfd, rewrite(pathname, buf, sizeof buf), st, flags);
}

typedef int (*stat_t)(const char *, struct stat *);
int stat(const char *pathname, struct stat *st) {
  static stat_t real = NULL;
  if (!real) real = (stat_t)dlsym(RTLD_NEXT, "stat");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef int (*statv_t)(const char *, struct stat64 *);
int stat64(const char *pathname, struct stat64 *st) {
  static statv_t real = NULL;
  if (!real) real = (statv_t)dlsym(RTLD_NEXT, "stat64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

int lstat64(const char *pathname, struct stat64 *st) {
  static statv_t real = NULL;
  if (!real) real = (statv_t)dlsym(RTLD_NEXT, "lstat64");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), st);
}

typedef FILE *(*fopen_t)(const char *, const char *);
FILE *fopen(const char *pathname, const char *mode) {
  static fopen_t real = NULL;
  if (!real) real = (fopen_t)dlsym(RTLD_NEXT, "fopen");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

/* A maintainer script (or something it sources, like debconf's
 * confmodule calling out to /usr/lib/cdebconf/debconf) can also just
 * *run* an absolute path, not only open/read one -- access() and the
 * exec family need the same rewrite. */
typedef int (*access_t)(const char *, int);
int access(const char *pathname, int mode) {
  static access_t real = NULL;
  if (!real) real = (access_t)dlsym(RTLD_NEXT, "access");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}

typedef int (*execv_t)(const char *, char *const[]);
int execv(const char *pathname, char *const argv[]) {
  static execv_t real = NULL;
  if (!real) real = (execv_t)dlsym(RTLD_NEXT, "execv");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), argv);
}

typedef int (*execve_t)(const char *, char *const[], char *const[]);
int execve(const char *pathname, char *const argv[], char *const envp[]) {
  static execve_t real = NULL;
  if (!real) real = (execve_t)dlsym(RTLD_NEXT, "execve");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), argv, envp);
}
