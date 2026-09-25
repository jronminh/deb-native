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
 * dynamically-linked glibc binary uses to open/stat a file and rewrites
 * any path matching a configured prefix to a different prefix, before
 * handing it to the real libc function. No kernel privilege is needed —
 * this works entirely at userspace symbol interposition.
 *
 * Verified against a real gap: figlet-figlet (from figlet_2.2.5-3+b2,
 * Debian's own binary, no source patch) calls fstatat() on the hardcoded,
 * compiled-in absolute path /usr/share/figlet/standard.flf, with no
 * environment variable or CLI flag able to redirect it — exactly the case
 * sudo-less's own docs (view.md) cite as needing their view. With this
 * shim preloaded and TDB_REDIRECT_FROM=/usr/share/figlet
 * TDB_REDIRECT_TO=<prefix root> set, it prints the ASCII banner
 * correctly, reading the font from wherever it was actually unpacked.
 *
 * Current limitation: ONE prefix mapping per process (TDB_REDIRECT_FROM /
 * TDB_REDIRECT_TO), not a general list — enough to prove the mechanism,
 * not yet a real multi-package tool. See "Open work" in
 * docs/design-manual-overlay.md.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>

static const char *from_prefix(void) {
  const char *p = getenv("TDB_REDIRECT_FROM");
  return p ? p : "";
}
static const char *to_prefix(void) {
  const char *p = getenv("TDB_REDIRECT_TO");
  return p ? p : "";
}

static const char *rewrite(const char *path, char *buf, size_t bufsz) {
  const char *from = from_prefix();
  size_t flen = strlen(from);
  if (flen && path && strncmp(path, from, flen) == 0) {
    snprintf(buf, bufsz, "%s%s", to_prefix(), path);
    if (getenv("TDB_REDIRECT_DEBUG"))
      fprintf(stderr, "[path-redirect] %s -> %s\n", path, buf);
    return buf;
  }
  return path;
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

typedef FILE *(*fopen_t)(const char *, const char *);
FILE *fopen(const char *pathname, const char *mode) {
  static fopen_t real = NULL;
  if (!real) real = (fopen_t)dlsym(RTLD_NEXT, "fopen");
  char buf[4096];
  return real(rewrite(pathname, buf, sizeof buf), mode);
}
