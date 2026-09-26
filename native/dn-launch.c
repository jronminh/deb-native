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

  const char *pfx = getenv("DN_TERMUX_PREFIX");
  if (!pfx || !*pfx) pfx = getenv("PREFIX");
  if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";

  /* Fusion mode (branch experiment, not the default prefix design): the
   * self-location math below (up 3 dirs from .../usr/bin/dn-shell) and the
   * PATH/shim construction both assume the prefix design's own nested
   * usr/lib/deb-native tree, which doesn't exist when INSTDIR is Termux's
   * real, flat prefix. Rather than reworking that math for a layout this
   * binary was never placed into for this experiment, an explicit caller
   * that already knows both values (DN_INSTDIR + DN_FUSE_SHIM) is trusted
   * completely instead of self-locating -- e.g. set by hand before running
   * `dpkg --configure` on a repackaged .deb during this proof-of-concept.
   * DN_FUSE_USR is implied and forced on for the shim in this path. */
  const char *fuse_instdir = getenv("DN_INSTDIR");
  const char *fuse_shim = getenv("DN_FUSE_SHIM");
  int fuse = fuse_instdir && *fuse_instdir && fuse_shim && *fuse_shim;

  char inst[4096], shim[4096], path[8192];
  if (fuse) {
    snprintf(inst, sizeof inst, "%s", fuse_instdir);
    snprintf(shim, sizeof shim, "%s", fuse_shim);
  } else {
    /* True fusion installs this binary as
     * $INSTDIR/lib/deb-native/fusion-bin/dn-shell (scripts/dn-layout.sh), so
     * dpkg-run maintainer scripts self-locate with no env from the caller. */
    const char *mark = "/lib/deb-native/fusion-bin/";
    char *m = strstr(self, mark);
    if (m && !strchr(m + strlen(mark), '/')) {
      snprintf(inst, sizeof inst, "%.*s", (int)(m - self), self);
      snprintf(shim, sizeof shim, "%s/lib/deb-native/path-redirect.so", inst);
      fuse = 1;
    }
  }
  if (fuse) {
    /* fusion-bin comes first: it holds wrappers (scripts/fuse-runtime.sh)
     * for commands that are DPKG_ROOT-aware but ALSO have a compiled-in
     * absolute --altdir/--admindir-style default (update-alternatives --
     * docs/findings.md, "Bug 4"). $inst/bin here IS Termux's own real bin/
     * (fusion mode has no separate sandbox tree), so such a wrapper must
     * live somewhere that never collides with -- or shadows -- Termux's
     * own real binaries; it is not $inst/bin itself.
     * Termux's glibc tools come BEFORE $inst/bin: that is Termux's Bionic
     * coreutils, and a Bionic child never gets the shim (path-redirect.c
     * strips it for non-glibc targets), so `head /etc/x` from a maintainer
     * script would read the host path instead of the prefix's. The classic
     * layout gets this order for free: its $inst/bin holds no Bionic tools. */
    snprintf(path, sizeof path, "%s/lib/deb-native/fusion-bin:%s/glibc/bin:%s/bin:%s/games",
             inst, pfx, inst, inst);
    setenv("DN_FUSE_USR", "1", 1);
  } else {
    snprintf(inst, sizeof inst, "%s", self);
    up_dirs(inst, 3); /* .../usr/bin/dn-shell -> INSTDIR */
    snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/path-redirect.so", inst);
    snprintf(path, sizeof path,
             "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:%s/usr/games:%s/glibc/bin:%s/bin",
             inst, inst, inst, inst, inst, pfx, pfx);
  }

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
