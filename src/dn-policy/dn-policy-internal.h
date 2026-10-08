/* Internal plumbing shared between dn-policy.c and
 * dn-policy-fakeroot.c/dn-policy-hardlink.c. Not part of the public API
 * in dn-policy.h -- nothing outside this directory includes this file. */

#ifndef DN_POLICY_INTERNAL_H
#define DN_POLICY_INTERNAL_H

#include <sys/stat.h>

/* Called once from dn_policy_init(), after tree_root/rt_root are
 * validated, to set up the owner-store/fake-xattr state under
 * "<rt_root>/state/" and probe which owner-store backend to use. */
int dn_policy_fakeroot_init(const char *rt_root);

/* Called once from dn_policy_init(), to set up the link2symlink hidden
 * file directory and refcount DB under "<rt_root>/state/links/". */
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
