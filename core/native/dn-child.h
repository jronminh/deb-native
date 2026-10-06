/* dn-child.h — shared setup for the maintainer-script interpreters.
 *
 * `dn-sh` and `dn-perl` are separate tiny ELF binaries the kernel runs
 * from a maintainer script's shebang (`#!$INSTDIR/usr/bin/dn-sh`). A
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

static void dn_up_dirs(char *p, int n) {
  while (n-- > 0) {
    char *s = strrchr(p, '/');
    if (!s || s == p) return;
    *s = '\0';
  }
}

/* Derive $INSTDIR from our own path and set the child environment. Returns 0
 * on success, -1 if /proc/self/exe could not be read. inst must be at least
 * 4096 bytes. */
static int dn_prepare_child(char *inst, size_t sz) {
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) return -1;
  self[n] = '\0';
  snprintf(inst, sz, "%s", self);
  dn_up_dirs(inst, 3); /* .../usr/bin/dn-sh -> INSTDIR */

  char shim[4096];
  snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/dn-shim.so", inst);

  char path[8192];
  const char *home = getenv("HOME");
  snprintf(path, sizeof path,
           "%s/usr/lib/deb-native/priv:%s/usr/lib/deb-native/bin:"
           "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:"
           "%s/usr/games:%s/.local/bin",
           inst, inst, inst, inst, inst, inst, inst,
           (home && *home) ? home : "/nonexistent");

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
