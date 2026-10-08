/* deb-native: the identity group under fake root.  See syscall/dn-identity.h. */
#include <errno.h>		/* E*, */
#include <sys/types.h>		/* uid_t, gid_t, */

#include "syscall/dn-identity.h"
#include "syscall/sysnum.h"
#include "syscall/syscall.h"	/* dn_fake_root(), */
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "cli/note.h"
#include "dn-policy.h"

/* This tracee's ids, a fresh root login on first use (a child is given a
 * copy of its parent's, tracee/tracee.c).  */
static DnIdentity *identity(Tracee *tracee)
{
	if (tracee->dn_identity == NULL)
		tracee->dn_identity = dn_policy_identity_new();
	return tracee->dn_identity;
}

/* Write @value (a 32-bit id) at the tracee's @address.  */
static int put_id(Tracee *tracee, word_t address, uint32_t value)
{
	if (address == 0)
		return -EFAULT;
	return write_data(tracee, address, &value, sizeof(value)) < 0 ? -EFAULT : 0;
}

static long answer(Tracee *tracee, Sysnum sysnum, DnIdentity *id)
{
	word_t a1 = peek_reg(tracee, CURRENT, SYSARG_1);
	word_t a2 = peek_reg(tracee, CURRENT, SYSARG_2);
	word_t a3 = peek_reg(tracee, CURRENT, SYSARG_3);
	uid_t r, e, s;
	int status;

	switch (sysnum) {
	case PR_getuid:
		return dn_policy_fake_getuid(id);
	case PR_getgid:
		return dn_policy_fake_getgid(id);
	case PR_geteuid:
		dn_policy_fake_getresuid(id, &r, &e, &s);
		return e;
	case PR_getegid:
		dn_policy_fake_getresgid(id, &r, &e, &s);
		return e;

	case PR_getresuid:
	case PR_getresgid:
		if (sysnum == PR_getresuid)
			dn_policy_fake_getresuid(id, &r, &e, &s);
		else
			dn_policy_fake_getresgid(id, &r, &e, &s);
		if ((status = put_id(tracee, a1, r)) < 0
		    || (status = put_id(tracee, a2, e)) < 0
		    || (status = put_id(tracee, a3, s)) < 0)
			return status;
		return 0;

	case PR_setuid:
		return dn_policy_fake_setuid(id, (uid_t) a1);
	case PR_setgid:
		return dn_policy_fake_setgid(id, (gid_t) a1);
	case PR_setreuid:
		return dn_policy_fake_setreuid(id, (uid_t) a1, (uid_t) a2);
	case PR_setregid:
		return dn_policy_fake_setregid(id, (gid_t) a1, (gid_t) a2);
	case PR_setresuid:
		return dn_policy_fake_setresuid(id, (uid_t) a1, (uid_t) a2, (uid_t) a3);
	case PR_setresgid:
		return dn_policy_fake_setresgid(id, (gid_t) a1, (gid_t) a2, (gid_t) a3);
	case PR_setfsuid:
		return dn_policy_fake_setfsuid(id, (uid_t) a1);
	case PR_setfsgid:
		return dn_policy_fake_setfsgid(id, (gid_t) a1);

	case PR_getgroups: {
		gid_t list[64];
		int n = dn_policy_fake_getgroups(id, 0, NULL);

		if ((int) a1 < 0)
			return -EINVAL;
		if (a1 == 0)
			return n;
		n = dn_policy_fake_getgroups(id, sizeof(list) / sizeof(list[0]), list);
		if (n < 0)
			return n;
		if ((size_t) a1 < (size_t) n)
			return -EINVAL;
		if (n > 0 && write_data(tracee, a2, list, n * sizeof(gid_t)) < 0)
			return -EFAULT;
		return n;
	}

	case PR_setgroups: {
		gid_t list[64];

		if (a1 > sizeof(list) / sizeof(list[0]))
			return -EINVAL;
		if (a1 > 0 && read_data(tracee, list, a2, a1 * sizeof(gid_t)) < 0)
			return -EFAULT;
		return dn_policy_fake_setgroups(id, (size_t) a1, list);
	}

	default:
		return 1;	/* Not ours.  */
	}
}

int dn_identity_result(Tracee *tracee, Sysnum sysnum, long *result)
{
	DnIdentity *id;

	switch (sysnum) {
	case PR_getuid: case PR_geteuid: case PR_getgid: case PR_getegid:
	case PR_getresuid: case PR_getresgid: case PR_getgroups:
	case PR_setuid: case PR_setgid: case PR_setreuid: case PR_setregid:
	case PR_setresuid: case PR_setresgid: case PR_setgroups:
	case PR_setfsuid: case PR_setfsgid:
		break;
	default:
		return 1;
	}
	if (!dn_fake_root())
		return 1;

	id = identity(tracee);
	if (id == NULL)
		return -ENOMEM;

	*result = answer(tracee, sysnum, id);
	return 0;
}

int dn_identity_enter(Tracee *tracee, Sysnum sysnum)
{
	long result;
	int status;

	status = dn_identity_result(tracee, sysnum, &result);
	if (status != 0)
		return status;
	if (result < 0)
		return (int) result;

	/* Answered here; the kernel never sees it.  */
	poke_reg(tracee, SYSARG_RESULT, (word_t) result);
	set_sysnum(tracee, PR_void);
	return 0;
}
