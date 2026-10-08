/* deb-native: file ownership under fake root.  See syscall/dn-owner.h. */
#include <errno.h>		/* E*, */
#include <fcntl.h>		/* AT_*, */
#include <limits.h>		/* PATH_MAX, */
#include <stdio.h>		/* snprintf(3), */
#include <string.h>		/* str*(3), */
#include <unistd.h>		/* readlink(2), */
#include <sys/stat.h>		/* struct stat, lstat(2), */
#include <sys/sysmacros.h>	/* makedev(3), */
#include <linux/stat.h>		/* struct statx, */

#include "syscall/dn-owner.h"
#include "syscall/syscall.h"	/* get_sysarg_path(), dn_fake_root(), */
#include "syscall/sysnum.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "path/path.h"		/* readlink_proc_pid_fd(), join_paths(), */
#include "dn-policy.h"

/* The host path of the file a syscall named: its (already translated)
 * path argument, or the file behind its fd -- @fd_reg's descriptor with
 * no path (fchown, fstat), or the dirfd for an empty path with
 * AT_EMPTY_PATH, or the dirfd/cwd a relative path is under.  At the exit
 * stage the first argument register holds the result, so the descriptor
 * comes from the registers saved at the enter stage.  */
static int host_path_in(Tracee *tracee, RegVersion version, Reg fd_reg, int path_reg,
			char out[PATH_MAX])
{
	char path[PATH_MAX];
	char base[PATH_MAX];
	char link[64];
	int fd = (int) peek_reg(tracee, version, fd_reg);
	int status;

	if (path_reg < 0)
		return readlink_proc_pid_fd(tracee->pid, fd, out);

	status = get_sysarg_path(tracee, path, (Reg) path_reg);
	if (status < 0)
		return status;
	if (path[0] == '/') {
		strcpy(out, path);
		return 0;
	}
	if (path[0] == '\0')
		return readlink_proc_pid_fd(tracee->pid, fd, out);

	if (fd == AT_FDCWD) {
		snprintf(link, sizeof(link), "/proc/%d/cwd", tracee->pid);
		status = readlink(link, base, sizeof(base) - 1);
		if (status < 0)
			return -errno;
		base[status] = '\0';
	}
	else {
		status = readlink_proc_pid_fd(tracee->pid, fd, base);
		if (status < 0)
			return status;
	}
	return join_paths(2, out, base, path);
}

/* At the exit stage (see above).  */
static int host_path_of(Tracee *tracee, Reg fd_reg, int path_reg, char out[PATH_MAX])
{
	return host_path_in(tracee, MODIFIED, fd_reg, path_reg, out);
}

/* The (dev, ino) of @host_path, following a final symlink unless
 * @nofollow -- the file the syscall acted on.  */
static int identify(const char *host_path, bool nofollow, struct stat *st)
{
	return (nofollow ? lstat(host_path, st) : stat(host_path, st)) < 0 ? -errno : 0;
}

/* chown family: record the owner; the real call was refused or a no-op.  */
static void owner_exit(Tracee *tracee, Sysnum sysnum, word_t result)
{
	char host[PATH_MAX];
	struct stat st;
	uint32_t uid, gid;
	bool nofollow = false;
	int status;

	if ((int) result != 0 && (int) result != -EPERM)
		return;

	if (sysnum == PR_fchown) {
		status = host_path_of(tracee, SYSARG_1, -1, host);
		uid = peek_reg(tracee, MODIFIED, SYSARG_2);
		gid = peek_reg(tracee, MODIFIED, SYSARG_3);
	}
	else {
		status = host_path_of(tracee, SYSARG_1, SYSARG_2, host);
		uid = peek_reg(tracee, MODIFIED, SYSARG_3);
		gid = peek_reg(tracee, MODIFIED, SYSARG_4);
		nofollow = (peek_reg(tracee, MODIFIED, SYSARG_5) & AT_SYMLINK_NOFOLLOW) != 0;
	}
	if (status < 0 || identify(host, nofollow, &st) < 0)
		return;	/* Leave the kernel's answer.  */

	/* A symlink itself carries no user.* xattr (the store would land on
	 * its target), and a symlink reads as root's anyway: succeed.  */
	if (S_ISLNK(st.st_mode)) {
		poke_reg(tracee, SYSARG_RESULT, 0);
		return;
	}

	status = dn_policy_owner_merge(host, st.st_dev, st.st_ino, 1, uid, gid, 0, 0);
	poke_reg(tracee, SYSARG_RESULT, (word_t) (status < 0 ? -EIO : 0));
}

/* chmod family: the ordinary bits are real (the kernel just set them);
 * setuid/setgid go to the store, as fake root keeps them.  */
static void mode_exit(Tracee *tracee, Sysnum sysnum, word_t result)
{
	char host[PATH_MAX];
	struct stat st;
	uint32_t mode;
	int status;

	if ((int) result != 0)
		return;

	if (sysnum == PR_fchmod) {
		status = host_path_of(tracee, SYSARG_1, -1, host);
		mode = peek_reg(tracee, MODIFIED, SYSARG_2);
	}
	else {
		status = host_path_of(tracee, SYSARG_1, SYSARG_2, host);
		mode = peek_reg(tracee, MODIFIED, SYSARG_3);
	}
	if (status < 0 || identify(host, false, &st) < 0)
		return;
	(void) dn_policy_owner_merge(host, st.st_dev, st.st_ino, 0, 0, 0, 1, mode);
}

/* stat family: owners, setuid bits and hardlinks as the store says.  */
static void stat_exit(Tracee *tracee, Sysnum sysnum, word_t result)
{
	char host[PATH_MAX];
	word_t buffer;
	int status;

	if ((int) result != 0)
		return;

	if (sysnum == PR_fstat) {
		status = host_path_of(tracee, SYSARG_1, -1, host);
		buffer = peek_reg(tracee, MODIFIED, SYSARG_2);
	}
	else if (sysnum == PR_statx) {
		status = host_path_of(tracee, SYSARG_1, SYSARG_2, host);
		buffer = peek_reg(tracee, MODIFIED, SYSARG_5);
	}
	else {
		status = host_path_of(tracee, SYSARG_1, SYSARG_2, host);
		buffer = peek_reg(tracee, MODIFIED, SYSARG_3);
	}
	if (status < 0)
		return;

	if (sysnum == PR_statx) {
		struct statx stx;
		struct stat st;

		if (read_data(tracee, &stx, buffer, sizeof(stx)) < 0)
			return;
		memset(&st, 0, sizeof(st));
		st.st_dev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
		st.st_ino = stx.stx_ino;
		st.st_mode = stx.stx_mode;
		st.st_nlink = stx.stx_nlink;
		st.st_uid = stx.stx_uid;
		st.st_gid = stx.stx_gid;
		st.st_size = stx.stx_size;
		dn_policy_fake_stat(host, &st);
		stx.stx_ino = st.st_ino;
		stx.stx_mode = st.st_mode;
		stx.stx_nlink = st.st_nlink;
		stx.stx_uid = st.st_uid;
		stx.stx_gid = st.st_gid;
		stx.stx_size = st.st_size;
		(void) write_data(tracee, buffer, &stx, sizeof(stx));
	}
	else {
		struct stat st;	/* arm64: the kernel's struct stat is glibc's.  */

		if (read_data(tracee, &st, buffer, sizeof(st)) < 0)
			return;
		dn_policy_fake_stat(host, &st);
		(void) write_data(tracee, buffer, &st, sizeof(st));
	}
}

/* linkat: SELinux refuses a real hardlink in app data -- link2symlink
 * instead (dn-policy, the same as dn-glibc's link()).  */
static void link_exit(Tracee *tracee, word_t result)
{
	char from[PATH_MAX];
	char to[PATH_MAX];
	int status;

	if ((int) result != -EACCES && (int) result != -EPERM)
		return;
	if (host_path_of(tracee, SYSARG_1, SYSARG_2, from) < 0
	    || host_path_of(tracee, SYSARG_3, SYSARG_4, to) < 0)
		return;
	status = dn_policy_link(from, to);
	poke_reg(tracee, SYSARG_RESULT, (word_t) (status < 0 ? status : 0));
}

int dn_owner_enter(Tracee *tracee, Sysnum sysnum)
{
	char host[PATH_MAX];

	if (!dn_fake_root())
		return 1;

	switch (sysnum) {
	case PR_mkdirat:
		/* Real root's DAC override, for the directories a program
		 * makes: dpkg creates one, fills it, and only then sets its
		 * mode (/etc/sudoers.d: 0750, made with no owner bits).  The
		 * owner keeps rwx; a later chmod sets the final mode.  */
		poke_reg(tracee, SYSARG_3, peek_reg(tracee, CURRENT, SYSARG_3) | S_IRWXU);
		return 0;

	case PR_unlinkat:
		/* A link2symlink name: drop its link count before the real
		 * unlink removes the name (dn-policy reads its target).  */
		if ((peek_reg(tracee, CURRENT, SYSARG_3) & AT_REMOVEDIR) != 0)
			return 0;
		if (host_path_in(tracee, CURRENT, SYSARG_1, SYSARG_2, host) == 0)
			(void) dn_policy_unlink(host);
		return 0;

	default:
		return 1;
	}
}

int dn_owner_exit(Tracee *tracee, Sysnum sysnum, word_t result)
{
	if (!dn_fake_root())
		return 1;

	switch (sysnum) {
	case PR_fchown:
	case PR_fchownat:
		owner_exit(tracee, sysnum, result);
		return 0;

	case PR_fchmod:
	case PR_fchmodat:
		mode_exit(tracee, sysnum, result);
		return 0;

	case PR_linkat:
		link_exit(tracee, result);
		return 0;

	case PR_fstat:
	case PR_fstatat64:	/* arm64's newfstatat (sysnums-arm64.h) */
	case PR_newfstatat:
	case PR_statx:
		stat_exit(tracee, sysnum, result);
		return 0;

	default:
		return 1;
	}
}
