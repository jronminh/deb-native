/* Probe for tests/ld-dn-config/run.sh: a plain glibc program whose ELF
 * interpreter is repointed at the built ld-dn. Prints the environment
 * entries the test asserts on, so ld-dn's resolved policy is visible
 * without a debug build. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const char *keys[] = {
    "LD_PRELOAD", "LD_LIBRARY_PATH", "DN_INSTDIR", "COMPILER_PATH",
    "DN_REDIRECT_PREFIXES", "LOCPATH", "MARKER", "ONLYPROBE", 0
  };
  for (int i = 0; keys[i]; i++) {
    const char *v = getenv(keys[i]);
    printf("%s=%s\n", keys[i], v ? v : "(unset)");
  }
  return 0;
}
