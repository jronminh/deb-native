/* Internal plumbing shared between dn-policy.c and
 * dn-policy-fakeroot.c/dn-policy-hardlink.c. Not part of the public API
 * in dn-policy.h -- nothing outside this directory includes this file. */

#ifndef DN_POLICY_INTERNAL_H
#define DN_POLICY_INTERNAL_H

#include <sys/stat.h>

/* @rt_root, once dn_policy_init() has set it, or NULL before that (or
 * if init failed). Lets dn-policy-fakeroot.c/dn-policy-hardlink.c
 * lazily init themselves from their own entry points, independently of
 * each other and of path mapping -- see dn_policy_init()'s comment on
 * why: each syscall only pays for the dn-policy subsystem it actually
 * needs (runtime.md principle 2), so dn_policy_init() itself sets up
 * path mapping only. */
const char *dn_policy_rt_root(void);

/* Sets up the owner-store/fake-xattr state under "<rt_root>/state/"
 * and probes which owner-store backend to use. Idempotent -- safe to
 * call on every dn-policy-fakeroot.c entry point; a second call after
 * success is a cheap no-op. Not called from dn_policy_init(); see
 * dn_policy_rt_root()'s comment. */
int dn_policy_fakeroot_init(const char *rt_root);

/* Sets up the link2symlink hidden file directory and refcount DB under
 * "<rt_root>/state/links/". Idempotent, not called from
 * dn_policy_init() -- same reasoning as dn_policy_fakeroot_init(). */
int dn_policy_hardlink_init(const char *rt_root);

/* Used by dn-policy-fakeroot.c's dn_policy_fake_stat(), before the
 * owner-store lookup (so that lookup sees the hidden file's real
 * (dev, ino), identical for every hardlinked name -- matching real
 * hardlink semantics, where one owner applies to every name). If
 * @host_path is a managed link2symlink name, *@st is replaced wholesale
 * with the hidden file's own real stat() (this is needed under lstat()
 * too, not just stat() -- an lstat() on a real hardlink never differs
 * from stat(), so a managed name must behave the same way regardless of
 * which one the caller actually issued) and st_nlink is overridden from
 * the stored refcount. Returns 1 if it did that, 0 if @host_path is not
 * a managed name (leaves *@st untouched), or a negative errno on an
 * unexpected failure reading the link or the refcount DB. */
int dn_policy_hardlink_fixup_stat(const char *host_path, struct stat *st);

#endif /* DN_POLICY_INTERNAL_H */
