/* dn-launch — the maintainer-script interpreter, as a real ELF binary.
 *
 * Replaces the earlier `dn-sh` being a shell script. A maintainer script
 * whose shebang points at a *script* does not work: the kernel follows only
 * one `#!` level, so `#!$INSTDIR/usr/bin/dn-sh` where dn-sh is itself
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
 *     ($INSTDIR/usr/bin/dn-sh -> up three dirs),
 *   - sets LD_PRELOAD (the dn-shim shim, a copy under
 *     $INSTDIR/lib/deb-native/), DN_INSTDIR, PATH (prefix first, then
 *     Termux's glibc coreutils, then Termux's own Bionic bin) and
 *     DEBIAN_FRONTEND,
 *   - execs the prefix's own `bash` (Debian's real package, apt-installed
 *     like any other -- the same translation/loader pipeline as everything
 *     else the prefix runs, not a binary borrowed from outside it) with the
 *     original argv, so the maintainer script runs. Falls back to Termux's
 *     glibc bash only if the prefix's own is not yet installed (true during
 *     early bootstrap, before `bash` reaches the base package set) -- once
 *     it is, dn-sh is standing on the prefix's own foundation, not an
 *     external one, matching how every other glibc program here works.
 *     `dn-perl` still execs Termux's own perl deliberately (not a fallback
 *     gap): Termux's Perl is 5.42, trixie's `perl` package builds modules
 *     for 5.40 (TODO.md, 0.4.0 section) -- an open, separately-tracked
 *     version question, not an oversight like bash's was.
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
#include <sys/stat.h>

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
  up_dirs(inst, 3); /* .../usr/bin/dn-sh -> INSTDIR */

  const char *pfx = getenv("DN_TERMUX_PREFIX");
  if (!pfx || !*pfx) pfx = getenv("PREFIX");
  if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";

  char shim[4096];
  snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/dn-shim.so", inst);
  char path[8192];
  /* priv/ first: the privilege layer (no-op chown/chgrp/dpkg-statoverride,
   * the update-alternatives and dpkg-divert wrappers) must win over any
   * Debian package's real command of the same name (setup-runtime.sh).
   * bin/ next: the launchers (make-launchers.sh) win over the raw usr/bin
   * entries, so programs that need the tracer are routed there rather than
   * run directly -- this is what makes the userland session the default,
   * with no ~/.bashrc activation (docs/spec/userlands.md). */
  /* Prefix dirs first. Termux's glibc/Bionic dirs are appended ONLY while the
   * prefix has no dpkg yet (early bootstrap: libc6/libc-bin maintainer scripts
   * run through this shell and need Termux's cp/dpkg-trigger). Once the base
   * is installed the prefix is self-sufficient, so the steady-state PATH is
   * prefix-only -- no Termux entry (0.7.0 R1). $HOME/.local/bin is the
   * host-layer dir for our own prefix-independent commands (dn-list,
   * dn-switch). */
  {
    const char *home = getenv("HOME");
    snprintf(path, sizeof path,
             "%s/usr/lib/deb-native/priv:%s/usr/lib/deb-native/bin:"
             "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:"
             "%s/usr/games:%s/.local/bin",
             inst, inst, inst, inst, inst, inst, inst,
             (home && *home) ? home : "/nonexistent");
  }
  {
    char dpkg[4096];
    snprintf(dpkg, sizeof dpkg, "%s/usr/bin/dpkg", inst);
    if (access(dpkg, X_OK) != 0) {
      size_t l = strlen(path);
      snprintf(path + l, sizeof path - l, ":%s/glibc/bin:%s/bin", pfx, pfx);
    }
  }

  /* Preserve whatever preload we inherited (on Termux, termux-exec) so the
   * shim can hand it back to a Bionic child it execs -- see bionic_env()
   * in dn-shim.c. Never capture our own shim. Capture before we
   * overwrite LD_PRELOAD. Prefer the prefix's own vendored copy
   * (setup-runtime.sh) so a Bionic child needs nothing under Termux's tree
   * at runtime; the inherited path is a bootstrap-only fallback. */
  const char *inh = getenv("LD_PRELOAD");
  if (inh && *inh && !strstr(inh, "dn-shim.so")) {
    char hostpre[4096];
    snprintf(hostpre, sizeof hostpre,
             "%s/usr/lib/deb-native/host/libtermux-exec-ld-preload.so", inst);
    if (strstr(inh, "libtermux-exec") && access(hostpre, R_OK) == 0)
      setenv("DN_BIONIC_PRELOAD", hostpre, 1);
    else
      setenv("DN_BIONIC_PRELOAD", inh, 1);
  }

  setenv("LD_PRELOAD", shim, 1);
  setenv("DN_INSTDIR", inst, 1);
  setenv("PATH", path, 1);
  setenv("DEBIAN_FRONTEND", "noninteractive", 1);
  /* Android has no writable /tmp; give the userland the prefix's own tmp so
   * programs that honour TMPDIR (apt) work. /tmp itself is not redirected by
   * the shim, so setting the variable is the portable fix. */
  if (!getenv("TMPDIR")) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s/tmp", inst);
    mkdir(tmp, 0777);
    setenv("TMPDIR", tmp, 1);
  }

  const char *base = strrchr(self, '/');
  base = base ? base + 1 : self;

  if (strcmp(base, "dn-perl") == 0) {
    char perl[4096], p5[8192];
    /* The prefix's own perl when installed (0.7.0); Termux's glibc perl only
     * as a bootstrap fallback (the minimal seed has no perl). */
    snprintf(perl, sizeof perl, "%s/usr/bin/perl", inst);
    if (access(perl, X_OK) != 0)
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
  snprintf(bash, sizeof bash, "%s/usr/bin/bash", inst);
  if (access(bash, X_OK) != 0)
    snprintf(bash, sizeof bash, "%s/glibc/bin/bash", pfx);
  execv(bash, argv);
  perror("dn-sh: execv");
  return 127;
}
