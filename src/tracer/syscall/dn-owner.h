/* deb-native: file ownership under fake root, on the traced path, by
 * dn-policy's owner store -- the same store dn-glibc's chown()/chmod()
 * and stat() wrappers use in-process (docs/spec/overlay.md, "Fake root").
 * chown/fchown/fchownat record the owner and succeed; chmod's
 * setuid/setgid bits are recorded; a stat/fstat/statx result reports the
 * stored owner and bits (no record reads as root:root) and a link2symlink
 * hardlink as the file it stands for; a hardlink SELinux refuses is made
 * by link2symlink (dn-policy, as dn-glibc's link()).
 */
#ifndef DN_OWNER_H
#define DN_OWNER_H

#include "tracee/tracee.h"
#include "syscall/sysnum.h"

/* At the exit stage of @sysnum, whose kernel result is @result: rewrite
 * what fake root changes; a refused linkat becomes a link2symlink.
 * Returns 0 when @sysnum is one of these, 1 otherwise (or when fake root
 * is off).  */
int dn_owner_exit(Tracee *tracee, Sysnum sysnum, word_t result);

/* At the enter stage, after the path arguments are translated: unlinkat's
 * link2symlink bookkeeping.  Returns 0 when handled, 1 otherwise.  */
int dn_owner_enter(Tracee *tracee, Sysnum sysnum);

#endif /* DN_OWNER_H */
