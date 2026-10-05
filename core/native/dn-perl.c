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
  char perl[4096], p5[8192];
  snprintf(perl, sizeof perl, "%s/usr/bin/perl", inst);
  snprintf(p5, sizeof p5,
           "%s/usr/share/perl5:%s/usr/lib/aarch64-linux-gnu/perl5",
           inst, inst);
  setenv("PERL5LIB", p5, 1);
  execv(perl, argv);
  perror("dn-perl: execv");
  return 127;
}
