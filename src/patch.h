/* patch.h -- the svc-site patcher, shared by dn-exec (birth) and later dn-init.
 * patch_image patches the path syscalls' svc sites in [lo, hi) to a stub that
 * maps each path through the one law and re-issues the call on the lane. */
#ifndef PATCH_H
#define PATCH_H
#include <stdint.h>
void patch_image(uint64_t lo, uint64_t hi);
#endif
