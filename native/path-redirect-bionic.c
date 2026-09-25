/* Bionic build of the manual-overlay idea (docs/design-manual-overlay.md),
 * for maintainer scripts — path-redirect.c only helps glibc binaries;
 * dpkg's maintainer scripts run via Termux's own Bionic /bin/sh under
 * --force-script-chrootless, which needs its own build of this shim.
 *
 * Generalized from path-redirect.c's single FROM/TO pair to the same
 * four directories sudo-less's "view" overlaid: /usr, /etc, /var, /opt,
 * all rewritten under one DN_INSTDIR root. LD_PRELOAD survives into
 * maintainer scripts unchanged — confirmed by direct test (a throwaway
 * postinst dumping its own env showed LD_PRELOAD verbatim; dpkg does not
 * clear the environment before exec'ing a script).
 *
 * Build: plain `clang -fPIC -shared -o path-redirect-bionic.so
 * path-redirect-bionic.c -ldl` — no cross-compile flags needed, Bionic is
 * Termux's own native target.
 *
 * Usage: LD_PRELOAD=path-redirect-bionic.so DN_INSTDIR=/path/to/prefix/root <cmd>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>

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
        fprintf(stderr, "[path-redirect-bionic] %s -> %s\n", path, buf);
      return buf;
    }
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

typedef int (*symlink_t)(const char *, const char *);
int symlink(const char *target, const char *linkpath) {
  static symlink_t real = NULL;
  if (!real) real = (symlink_t)dlsym(RTLD_NEXT, "symlink");
  char buf[4096];
  return real(target, rewrite(linkpath, buf, sizeof buf));
}
