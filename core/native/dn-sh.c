/* dn-sh — the maintainer-script interpreter that runs the prefix's bash.
 *
 * The kernel runs this from a maintainer script's shebang
 * (`#!$INSTDIR/usr/bin/dn-sh`); shared setup is in dn-child.h. The
 * prefix's own `bash` is exec'd with the original argv. A prefix always has
 * its bash before any maintainer script runs (the bootstrap installs it first).
 */
#include "dn-child.h"

int main(int argc, char **argv) {
  (void)argc;
  char inst[4096];
  if (dn_prepare_child(inst, sizeof inst) != 0) {
    fprintf(stderr, "dn-sh: readlink /proc/self/exe failed\n");
    return 127;
  }
  char bash[4096], lp[8192];
  snprintf(bash, sizeof bash, "%s/usr/bin/bash", inst);
  /* Let the prefix's loader find the prefix's libraries. */
  const char *e = getenv("LD_LIBRARY_PATH");
  snprintf(lp, sizeof lp, "%s/usr/lib/aarch64-linux-gnu:%s/usr/lib%s%s",
           inst, inst, (e && *e) ? ":" : "", (e && *e) ? e : "");
  setenv("LD_LIBRARY_PATH", lp, 1);
  execv(bash, argv);
  perror("dn-sh: execv");
  return 127;
}
