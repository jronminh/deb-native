/* dn-policy -- path rewriting. See dn-policy.h for the calling
 * convention this follows and docs/spec/runtime.md, "dn-policy" >
 * "Path rewriting" for the rules implemented here.
 *
 * This file implements only dn_policy_init() and the three
 * translate/detranslate functions (longest-prefix mapping, the
 * /proc,/sys,/dev passthrough, no-double-translation). Absolute
 * in-tree symlink resolution (runtime.md: "Symlink tuyệt đối trong
 * cây... phải tự giải từng thành phần") is NOT implemented yet --
 * every function here is correct for a tree with no such symlinks and
 * will stay correct once symlink resolution is added on top, since
 * that only ever narrows a path further, never changes this mapping.
 * Fake root and hardlinks (dn-policy.h's other two sections) are a
 * separate increment.
 */

#include "dn-policy.h"
#include "dn-policy-internal.h"

#include <string.h>
#include <errno.h>

/* Sixteen bytes of slack on top of PATH_MAX: a guest path can be up to
 * PATH_MAX-1 long on its own, and prefixing it with tree_root must
 * still be checked, not silently truncated. */
#define DN_PATH_BUF (PATH_MAX + 16)

static char tree_root[PATH_MAX];
static size_t tree_root_len;
static char rt_root[PATH_MAX];
static size_t rt_root_len;
static int initialized;

/* runtime.md: "Ngoại lệ đi thẳng ra hệ thật: /proc, /sys, /dev." Fixed
 * for now; runtime.md says this list belongs in a config file, which is
 * a P4-ish refinement once there's an actual file format to read it
 * from -- these three are architecture/kernel-fixed, not prefix-fixed,
 * so hardcoding them is not a principle-5 violation the way a
 * prefix-specific value would be. */
static const char *const passthrough[] = { "/proc", "/sys", "/dev" };
#define N_PASSTHROUGH (sizeof(passthrough) / sizeof(passthrough[0]))

/* True if @path equals @prefix, or starts with @prefix followed by a
 * '/'. @prefix_len excludes any trailing '/'. Rejects a false match
 * like "/data" against prefix "/dat". */
static int has_path_prefix(const char *path, const char *prefix, size_t prefix_len)
{
	if (strncmp(path, prefix, prefix_len) != 0)
		return 0;
	return path[prefix_len] == '\0' || path[prefix_len] == '/';
}

static int copy_out(const char *src, char *out, size_t cap)
{
	size_t len = strlen(src);

	if (len + 1 > cap)
		return -ENAMETOOLONG;
	memcpy(out, src, len + 1);
	return 0;
}

int dn_policy_init(const char *tree_root_in, const char *rt_root_in)
{
	size_t len;
	int status;

	if (tree_root_in == NULL || tree_root_in[0] != '/')
		return -EINVAL;
	if (rt_root_in == NULL || rt_root_in[0] != '/')
		return -EINVAL;

	len = strlen(tree_root_in);
	if (len == 0 || len >= sizeof(tree_root))
		return -ENAMETOOLONG;
	/* No trailing slash, so has_path_prefix()'s boundary check is
	 * meaningful (a bare "/" tree_root is the one legal exception:
	 * every absolute path is then "under" it, which is correct). */
	if (len > 1 && tree_root_in[len - 1] == '/')
		return -EINVAL;

	memcpy(tree_root, tree_root_in, len + 1);
	tree_root_len = (len == 1 /* "/" */) ? 0 : len;

	len = strlen(rt_root_in);
	if (len == 0 || len >= sizeof(rt_root))
		return -ENAMETOOLONG;
	if (len > 1 && rt_root_in[len - 1] == '/')
		return -EINVAL;
	if (!has_path_prefix(rt_root_in, tree_root, tree_root_len))
		return -EINVAL; /* RT must sit inside TREE, see runtime.md */

	memcpy(rt_root, rt_root_in, len + 1);
	rt_root_len = len;

	status = dn_policy_fakeroot_init(rt_root);
	if (status != 0)
		return status;

	initialized = 1;
	return 0;
}

static int translate_path_common(const char *guest_path, char *host_path_out, size_t cap)
{
	char buf[DN_PATH_BUF];
	size_t guest_len;
	size_t i;

	if (!initialized)
		return -EINVAL;
	if (guest_path == NULL || guest_path[0] != '/')
		return -EINVAL;

	/* Already a host path (TREE covers RT too, since RT sits inside
	 * TREE) -- "Dịch không được lặp."  */
	if (has_path_prefix(guest_path, tree_root, tree_root_len))
		return copy_out(guest_path, host_path_out, cap);

	/* /proc, /sys, /dev: shared with the real system unchanged.  */
	for (i = 0; i < N_PASSTHROUGH; i++) {
		size_t plen = strlen(passthrough[i]);
		if (has_path_prefix(guest_path, passthrough[i], plen))
			return copy_out(guest_path, host_path_out, cap);
	}

	/* guest_path == "/": tree_root itself, not "tree_root/" (which
	 * has_path_prefix() would still match later, but a bare root is
	 * cleaner with no trailing slash to begin with).  */
	if (guest_path[1] == '\0')
		return copy_out(tree_root, host_path_out, cap);

	guest_len = strlen(guest_path);
	if (tree_root_len + guest_len + 1 > sizeof(buf))
		return -ENAMETOOLONG;

	memcpy(buf, tree_root, tree_root_len);
	memcpy(buf + tree_root_len, guest_path, guest_len + 1);

	return copy_out(buf, host_path_out, cap);
}

int dn_policy_translate_path(const char *guest_path, char *host_path_out, size_t cap)
{
	/* No symlink resolution yet (see file header) -- same as the
	 * nofollow variant until that lands.  */
	return translate_path_common(guest_path, host_path_out, cap);
}

int dn_policy_translate_path_nofollow(const char *guest_path, char *host_path_out, size_t cap)
{
	return translate_path_common(guest_path, host_path_out, cap);
}

int dn_policy_detranslate_path(const char *host_path, char *guest_path_out, size_t cap)
{
	const char *rest;

	if (!initialized)
		return -EINVAL;
	if (host_path == NULL || host_path[0] != '/')
		return -EINVAL;

	if (has_path_prefix(host_path, tree_root, tree_root_len)) {
		rest = host_path + tree_root_len;
		if (rest[0] == '\0')
			return copy_out("/", guest_path_out, cap);
		return copy_out(rest, guest_path_out, cap);
	}

	/* /proc, /sys, /dev: shared, unchanged (see translate_path_common).  */
	{
		size_t i;
		for (i = 0; i < N_PASSTHROUGH; i++) {
			size_t plen = strlen(passthrough[i]);
			if (has_path_prefix(host_path, passthrough[i], plen))
				return copy_out(host_path, guest_path_out, cap);
		}
	}

	return -ENOENT;
}
