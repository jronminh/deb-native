/* dn-child.h — shared setup for the maintainer-script interpreters.
 *
 * `dn-run` needs the same environment set up before it execs a
 * program. A
 * shebang interpreter must be a real ELF: the kernel follows only one `#!`
 * level, so an interpreter that is itself a script leaves dpkg falling back
 * to Android's Bionic /bin/sh with no shim and no glibc PATH -- surfacing as
 *   CANNOT LINK EXECUTABLE "/bin/sh": library "libc.so.6" not found.
 *
 * Both binaries derive $INSTDIR from their own /proc/self/exe
 * ($INSTDIR/usr/bin/<name> -> up three dirs), set the shim via LD_PRELOAD,
 * DN_INSTDIR, PATH (priv/ and bin/ first) and TMPDIR, then exec the
 * prefix's own program (bash / perl) with the original argv, so the
 * maintainer script runs.
 */
#ifndef DN_CHILD_H
#define DN_CHILD_H

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* The prefix root, from this program's own path: .../usr/<dir>/<prog> -> root.
 * Shared by the overlay programs that exec the prefix's own programs (dn-run). Returns 0 on
 * success, -1 if /proc/self/exe cannot be read. out must hold 4096 bytes. */
static inline int dn_derive_instdir(char *out, size_t sz) {
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) return -1;
  self[n] = '\0';
  /* Strip components until the one named "usr" is reached; the root is what
   * comes before it. Works for usr/bin/<prog> and usr/lib/deb-native/<prog>. */
  for (;;) {
    char *s = strrchr(self, '/');
    if (!s || s == self) return -1;
    *s = '\0';
    size_t l = strlen(self);
    if (l >= 4 && strcmp(self + l - 4, "/usr") == 0) {
      self[l - 4] = '\0';
      break;
    }
  }
  if (!self[0]) return -1;
  snprintf(out, sz, "%s", self);
  return 0;
}

/* The PATH every prefix program runs with: the privilege layer first, then the
 * launchers, then the prefix's own bin dirs and the user's ~/.local/bin. No
 * host directory is ever on it. */
static inline void dn_build_path(const char *inst, char *path, size_t sz) {
  const char *home = getenv("HOME");
  snprintf(path, sz,
           "%s/usr/lib/deb-native/priv:%s/usr/lib/deb-native/bin:"
           "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:"
           "%s/usr/games:%s/.local/bin",
           inst, inst, inst, inst, inst, inst, inst,
           (home && *home) ? home : "/nonexistent");
}

/* Derive $INSTDIR and set the environment of a child.
 * Returns 0 on success, -1 if /proc/self/exe could not be read. inst must be
 * at least 4096 bytes. */
static inline int dn_prepare_child(char *inst, size_t sz) {
  if (dn_derive_instdir(inst, sz) != 0) return -1;

  char shim[4096], path[8192];
  snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/dn-shim.so", inst);
  dn_build_path(inst, path, sizeof path);

  setenv("LD_PRELOAD", shim, 1);
  setenv("DN_INSTDIR", inst, 1);
  setenv("PATH", path, 1);
  setenv("DEBIAN_FRONTEND", "noninteractive", 1);

  if (!getenv("TMPDIR")) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s/tmp", inst);
    mkdir(tmp, 0777);
    setenv("TMPDIR", tmp, 1);
  }
  return 0;
}

#endif
