/* dn-launch — the maintainer-script interpreter, as a real ELF binary.
 *
 * Replaces the earlier `dn-shell` being a shell script. A maintainer script
 * whose shebang points at a *script* does not work: the kernel follows only
 * one `#!` level, so `#!$INSTDIR/usr/bin/dn-shell` where dn-shell is itself
 * `#!/system/bin/sh` leaves the kernel with nothing executable, dpkg falls
 * back to running the script under Android's Bionic /bin/sh, and the whole
 * glibc shim/PATH setup is skipped -- surfacing as the misleading
 *   CANNOT LINK EXECUTABLE "/bin/sh": library "libc.so.6" not found
 * (Bionic's linker aborting on the glibc .so that leaked into the env).
 *
 * This is a tiny Bionic executable (built natively by Termux clang, no
 * cross-toolchain needed). The kernel can execute it directly from a
 * shebang, and it then:
 *   - derives $INSTDIR from its own /proc/self/exe
 *     ($INSTDIR/usr/bin/dn-shell -> up three dirs),
 *   - sets LD_PRELOAD (the path-redirect shim, a copy under
 *     $INSTDIR/lib/deb-native/), DN_INSTDIR, PATH (prefix first, then
 *     Termux's glibc coreutils, then Termux's own Bionic bin) and
 *     DEBIAN_FRONTEND,
 *   - execs Termux's already-present glibc bash (or perl, when invoked as
 *     `dn-perl`) with the original argv, so the maintainer script runs.
 *
 * Bionic is used for this launcher on purpose: it must start *without*
 * LD_PRELOAD (the kernel applies none when following a shebang anyway), so
 * it is safe; it only sets LD_PRELOAD for the glibc child it execs.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void up_dirs(char *p, int n) {
  while (n-- > 0) {
    char *s = strrchr(p, '/');
    if (!s || s == p) return;
    *s = '\0';
  }
}

int main(int argc, char **argv) {
  (void)argc;
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) { fprintf(stderr, "dn-launch: readlink /proc/self/exe failed\n"); return 127; }
  self[n] = '\0';

  char inst[4096];
  snprintf(inst, sizeof inst, "%s", self);
  up_dirs(inst, 3); /* .../usr/bin/dn-shell -> INSTDIR */

  const char *pfx = getenv("DN_TERMUX_PREFIX");
  if (!pfx || !*pfx) pfx = getenv("PREFIX");
  if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";

  char shim[4096];
  snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/path-redirect.so", inst);
  char path[8192];
  snprintf(path, sizeof path,
           "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:%s/glibc/bin:%s/bin",
           inst, inst, inst, inst, pfx, pfx);

  /* Preserve whatever preload we inherited (on Termux, termux-exec) so the
   * shim can hand it back to a Bionic child it execs -- see bionic_env()
   * in path-redirect.c. Never capture our own shim. Capture before we
   * overwrite LD_PRELOAD. */
  const char *inh = getenv("LD_PRELOAD");
  if (inh && *inh && !strstr(inh, "path-redirect.so"))
    setenv("DN_BIONIC_PRELOAD", inh, 1);

  setenv("LD_PRELOAD", shim, 1);
  setenv("DN_INSTDIR", inst, 1);
  setenv("PATH", path, 1);
  setenv("DEBIAN_FRONTEND", "noninteractive", 1);

  const char *base = strrchr(self, '/');
  base = base ? base + 1 : self;

  if (strcmp(base, "dn-perl") == 0) {
    char perl[4096], p5[8192];
    snprintf(perl, sizeof perl, "%s/glibc/bin/perl", pfx);
    snprintf(p5, sizeof p5,
             "%s/usr/share/perl5:%s/usr/lib/aarch64-linux-gnu/perl5:%s/usr/share/perl/5.36",
             inst, inst, inst);
    setenv("PERL5LIB", p5, 1);
    execv(perl, argv);
    perror("dn-perl: execv");
    return 127;
  }

  char bash[4096];
  snprintf(bash, sizeof bash, "%s/glibc/bin/bash", pfx);
  execv(bash, argv);
  perror("dn-shell: execv");
  return 127;
}
