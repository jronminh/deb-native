/* Internal plumbing shared between dn-policy.c and
 * dn-policy-fakeroot.c/dn-policy-hardlink.c. Not part of the public API
 * in dn-policy.h -- nothing outside this directory includes this file. */

#ifndef DN_POLICY_INTERNAL_H
#define DN_POLICY_INTERNAL_H

/* Called once from dn_policy_init(), after tree_root/rt_root are
 * validated, to set up the owner-store/fake-xattr state under
 * "<rt_root>/state/" and probe which owner-store backend to use. */
int dn_policy_fakeroot_init(const char *rt_root);

#endif /* DN_POLICY_INTERNAL_H */
