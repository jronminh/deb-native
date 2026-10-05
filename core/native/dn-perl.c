/* dn-perl — the maintainer-script interpreter that runs the prefix's perl.
 *
 * The kernel runs this from a maintainer script's shebang
 * (`#!$INSTDIR/usr/bin/dn-perl`); shared setup is in dn-child.h. The
 * prefix's own `perl` is exec'd with the original argv, with the prefix's
 * module dirs on PERL5LIB.
 */
#include "dn-child.h"

int main(int argc, char **argv) {
  (void)argc;
  char inst[4096];
  if (dn_prepare_child(inst, sizeof inst) != 0) {
    fprintf(stderr, "dn-perl: readlink /proc/self/exe failed\n");
    return 127;
  }
  char perl[4096], p5[8192], dpkg[4096];
  snprintf(perl, sizeof perl, "%s/usr/bin/perl", inst);
  snprintf(dpkg, sizeof dpkg, "%s/usr/bin/dpkg", inst);
  if (access(dpkg, X_OK) != 0 || access(perl, X_OK) != 0) {
    /* Bootstrap (no prefix dpkg yet): Termux's glibc perl. Steady state: the
     * prefix's own (the app model has no Termux, so the branch is then dead). */
    const char *pfx = getenv("DN_TERMUX_PREFIX");
    if (!pfx || !*pfx) pfx = getenv("PREFIX");
    if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";
    snprintf(perl, sizeof perl, "%s/glibc/bin/perl", pfx);
  } else {
    /* Prefix perl: help its loader find the prefix libs. */
    char lp[8192];
    const char *e = getenv("LD_LIBRARY_PATH");
    snprintf(lp, sizeof lp, "%s/usr/lib/aarch64-linux-gnu:%s/usr/lib%s%s",
             inst, inst, (e && *e) ? ":" : "", (e && *e) ? e : "");
    setenv("LD_LIBRARY_PATH", lp, 1);
  }
  snprintf(p5, sizeof p5,
           "%s/usr/share/perl5:%s/usr/lib/aarch64-linux-gnu/perl5",
           inst, inst);
  setenv("PERL5LIB", p5, 1);
  execv(perl, argv);
  perror("dn-perl: execv");
  return 127;
}
