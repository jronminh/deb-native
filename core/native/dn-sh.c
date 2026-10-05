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
  char bash[4096], mark[4096];
  snprintf(bash, sizeof bash, "%s/usr/bin/bash", inst);
  snprintf(mark, sizeof mark, "%s/var/lib/deb-native/base-packages", inst);
  /* Bootstrap (base not installed yet): use Termux's glibc bash -- patched for
   * Android and with all libs present. Steady state: the prefix's own. */
  if (access(mark, R_OK) != 0 || access(bash, X_OK) != 0) {
    const char *pfx = getenv("DN_TERMUX_PREFIX");
    if (!pfx || !*pfx) pfx = getenv("PREFIX");
    if (!pfx || !*pfx) pfx = "/data/data/com.termux/files/usr";
    snprintf(bash, sizeof bash, "%s/glibc/bin/bash", pfx);
  } else {
    /* Prefix bash: let its loader find the prefix libs before ldconfig has
     * refreshed the prefix cache. */
    char lp[8192];
    const char *e = getenv("LD_LIBRARY_PATH");
    snprintf(lp, sizeof lp, "%s/usr/lib/aarch64-linux-gnu:%s/usr/lib%s%s",
             inst, inst, (e && *e) ? ":" : "", (e && *e) ? e : "");
    setenv("LD_LIBRARY_PATH", lp, 1);
  }
  execv(bash, argv);
  perror("dn-sh: execv");
  return 127;
}
