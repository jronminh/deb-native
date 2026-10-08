/* deb-native: the identity group (get*id, set*id, getgroups, setgroups,
 * setfs*id) under fake root, answered from the tracee's DnIdentity by
 * dn-policy's rules (docs/spec/overlay.md, "Fake root").  The call never
 * reaches the kernel: the ids are recorded, nothing real changes, and a
 * program that dropped root cannot take it back.
 */
#ifndef DN_IDENTITY_H
#define DN_IDENTITY_H

#include "tracee/tracee.h"
#include "syscall/sysnum.h"

/* Compute the answer to @sysnum (its arguments in @tracee's current
 * registers) into *@result, a value or -errno, if it belongs to the group
 * and fake root is on.  Returns 0 when answered, 1 when not ours, or
 * -ENOMEM.  Used by both stops a call can reach dn-trace through.  */
int dn_identity_result(Tracee *tracee, Sysnum sysnum, long *result);

/* At the enter stage: answer @sysnum (the result is poked and the syscall
 * voided).  Returns 0 when answered, 1 when not ours, or -errno to fail
 * the call.  Android's own filter traps the set*id calls with SIGSYS,
 * which wins over the shared filter's TRACE: those arrive at
 * tracee/seccomp.c instead, which uses dn_identity_result().  */
int dn_identity_enter(Tracee *tracee, Sysnum sysnum);

#endif /* DN_IDENTITY_H */
