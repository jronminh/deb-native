/* dn-sh — the maintainer-script interpreter that runs the prefix's bash.
 *
 * The kernel runs this from a maintainer script's shebang
 * (`#!$INSTDIR/usr/bin/dn-sh`); shared setup is in dn-child.h. The
 * prefix's own `bash` is exec'd with the original argv.
 */
#include "dn-child.h"

int main(int argc, char **argv) {
  (void)argc;
  char inst[4096];
  if (dn_prepare_child(inst, sizeof inst) != 0) {
    fprintf(stderr, "dn-sh: readlink /proc/self/exe failed\n");
    return 127;
  }
  char bash[4096], dpkg[4096];
  snprintf(bash, sizeof bash, "%s/usr/bin/bash", inst);
  snprintf(dpkg, sizeof dpkg, "%s/usr/bin/dpkg", inst);
  /* Bootstrap (no prefix dpkg yet): use Termux's glibc bash -- the prefix's
   * own bash may not have its libraries yet. Steady state: the prefix's. */
  if (access(dpkg, X_OK) != 0 || access(bash, X_OK) != 0) {
    const char *pfx = getenv("DN_TERMUX_PREFIX");
    if (!pfx || !*pfx) pfx = getenv("PREFIX");
    if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";
    snprintf(bash, sizeof bash, "%s/glibc/bin/bash", pfx);
  }
  execv(bash, argv);
  perror("dn-sh: execv");
  return 127;
}
