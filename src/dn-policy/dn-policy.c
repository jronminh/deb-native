/* dn-policy -- path rewriting. See dn-policy.h for the calling
 * convention this follows and docs/spec/runtime.md, "dn-policy" >
 * "Path rewriting" for the rules implemented here.
 *
 * This file implements dn_policy_init(), the three translate/
 * detranslate functions (longest-prefix mapping, the /proc,/sys,/dev
 * passthrough, no-double-translation), and absolute in-tree symlink
 * resolution (runtime.md: "Symlink tuyệt đối trong cây... phải tự
 * giải từng thành phần"). Not implemented yet: caching the resolution
 * (runtime.md says this too belongs here eventually; every lookup
 * below hits the real filesystem directly, correct but not free).
 * Fake root and hardlinks (dn-policy.h's other two sections) are
 * separate files.
 */

#include "dn-policy.h"
#include "dn-policy-internal.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

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
#define DN_MAX_BINDINGS 32

typedef struct {
	char host[PATH_MAX];
	char guest[PATH_MAX];
	size_t host_len;
	size_t guest_len;
} DnBinding;

static DnBinding bindings[DN_MAX_BINDINGS];
static size_t binding_count;

static int path_join(char *out, size_t cap, const char *a, const char *b);
static int has_path_prefix(const char *path, const char *prefix, size_t prefix_len);

static int binding_prefix(const char *path, const char *prefix, size_t len)
{
	return strncmp(path, prefix, len) == 0
		&& (path[len] == '\0' || path[len] == '/');
}

/* Longest host-side match: the tree root, or a row's android side. */
static size_t longest_host_match(const char *path)
{
	size_t best = has_path_prefix(path, tree_root, tree_root_len) ? tree_root_len : 0;
	size_t i;

	for (i = 0; i < binding_count; i++)
		if (bindings[i].host_len > best
		    && binding_prefix(path, bindings[i].host, bindings[i].host_len))
			best = bindings[i].host_len;
	return best;
}

/* Longest tree-side (guest) match, and the winning row index (or (size_t)-1). */
static size_t longest_guest_match(const char *path, size_t *index_out)
{
	size_t best = 0;
	size_t i;

	*index_out = (size_t)-1;
	for (i = 0; i < binding_count; i++)
		if (bindings[i].guest_len > best
		    && binding_prefix(path, bindings[i].guest, bindings[i].guest_len)) {
			best = bindings[i].guest_len;
			*index_out = i;
		}
	return best;
}

/* A path is already a host path when its longest host-side match is at least
 * as long as its longest tree-side match: a more specific guest row wins
 * (e.g. /dev/shm over the /dev passthrough row). */
static int already_host(const char *path, size_t *guest_index)
{
	size_t host_best = longest_host_match(path);
	size_t guest_best = longest_guest_match(path, guest_index);

	return host_best >= guest_best && host_best > 0;
}

/* Map @path through the row at @index into @out. Returns 0 or -errno. */
static int map_via_binding(size_t index, const char *path, char *out, size_t cap)
{
	size_t rest = strlen(path + bindings[index].guest_len);

	if (bindings[index].host_len + rest + 1 > cap)
		return -ENAMETOOLONG;
	memcpy(out, bindings[index].host, bindings[index].host_len);
	memcpy(out + bindings[index].host_len, path + bindings[index].guest_len, rest + 1);
	return 0;
}

static int add_binding(const char *host, const char *guest)
{
	DnBinding *b;
	size_t hl, gl;
	char canonical[PATH_MAX];
	const char *h = host;

	if (host == NULL || guest == NULL || host[0] != '/' || guest[0] != '/')
		return -EINVAL;
	/* Resolve symlinks on the host side so the shim produces the same host
	 * path the tracer does (PRoot canonicalizes binding hosts too, e.g.
	 * /sdcard -> /storage/emulated/0). */
	if (realpath(host, canonical) != NULL)
		h = canonical;
	host = h;
	hl = strlen(host);
	gl = strlen(guest);
	if (hl == 0 || gl == 0 || hl >= PATH_MAX || gl >= PATH_MAX)
		return -ENAMETOOLONG;
	if (hl > 1 && host[hl - 1] == '/')
		return -EINVAL;
	if (gl > 1 && guest[gl - 1] == '/')
		return -EINVAL;
	for (size_t i = 0; i < binding_count; i++) {
		if (strcmp(bindings[i].host, host) == 0
		    && strcmp(bindings[i].guest, guest) == 0)
			return 0;
	}
	if (binding_count >= DN_MAX_BINDINGS)
		return -ENOSPC;
	b = &bindings[binding_count++];
	memcpy(b->host, host, hl + 1);
	memcpy(b->guest, guest, gl + 1);
	b->host_len = hl;
	b->guest_len = gl;
	return 0;
}

static int load_bindings(const char *rt_root)
{
	char path[PATH_MAX];
	char line[PATH_MAX * 2 + 8];
	FILE *f;
	int status;

	binding_count = 0;
	status = add_binding("/proc", "/proc");
	if (status < 0) return status;
	status = add_binding("/sys", "/sys");
	if (status < 0) return status;
	status = add_binding("/dev", "/dev");
	if (status < 0) return status;
	status = path_join(path, sizeof(path), rt_root, "state/bindings");
	if (status < 0)
		return status;
	f = fopen(path, "r");
	if (f == NULL)
		return errno == ENOENT ? 0 : -errno;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *host, *guest, *end;

		line[strcspn(line, "\r\n")] = '\0';
		host = line;
		while (*host == ' ' || *host == '\t') host++;
		if (*host == '\0' || *host == '#') continue;
		guest = strchr(host, '\t');
		if (guest == NULL) {
			fclose(f);
			return -EINVAL;
		}
		*guest++ = '\0';
		end = guest + strcspn(guest, "\t ");
		*end = '\0';
		status = add_binding(host, guest);
		if (status < 0) {
			fclose(f);
			return status;
		}
	}
	status = ferror(f) ? -EIO : 0;
	fclose(f);
	return status;
}

size_t dn_policy_binding_count(void)
{
	return binding_count;
}

int dn_policy_binding_get(size_t index, const char **host, const char **guest)
{
	if (index >= binding_count || host == NULL || guest == NULL)
		return -EINVAL;
	*host = bindings[index].host;
	*guest = bindings[index].guest;
	return 0;
}

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

/* "<a>/<b>" into @out, bounded by @cap. memcpy/strlen only, deliberately
 * not snprintf: this file is also compiled into dn-shim
 * (sysdeps/unix/sysv/linux/), and snprintf pulls in glibc's full
 * vfprintf machinery (which touches malloc, locking, more) -- fine in
 * an ordinary program, but fatal here, because glibc's own
 * elf/librtld.map build step links the loader's minimal object set
 * against the *entire* libc_pic.a to discover what else it needs, and
 * hard-fails on any duplicate definition it finds doing so. Pulling in
 * real malloc/free/realloc (and flock's __syscall_cancel wrapper, and
 * sbrk, and __libc_fatal) that way is exactly such a duplicate --
 * confirmed by bisection, not guessed. dn-prefix.h's own __dn_build()/
 * __dn_redirect() already avoided snprintf for what was probably this
 * same reason. */
static int path_join(char *out, size_t cap, const char *a, const char *b)
{
	size_t al = strlen(a), bl = strlen(b);

	if (al + 1 + bl + 1 > cap)
		return -ENAMETOOLONG;
	memcpy(out, a, al);
	out[al] = '/';
	memcpy(out + al + 1, b, bl + 1);
	return 0;
}

/* In-place next '/'-separated, non-empty component of *cursor -- same
 * skip-empty-components behavior as strtok_r(s, "/", ...), but a
 * hand-rolled single-purpose scan instead: strtok_r is reentrant-safe
 * (fine), but it, like snprintf, is a general-purpose library function
 * whose internal cost and whatever it might call isn't something this
 * file should have to take on faith. Mutates the string in place (each
 * separator becomes '\0'), same as strtok_r. Returns NULL once
 * exhausted. */
static char *next_component(char **cursor)
{
	char *start = *cursor;
	char *p;

	while (*start == '/')
		start++;
	if (*start == '\0') {
		*cursor = start;
		return NULL;
	}

	p = start;
	while (*p != '\0' && *p != '/')
		p++;
	if (*p == '/') {
		*p = '\0';
		*cursor = p + 1;
	} else {
		*cursor = p;
	}
	return start;
}

/* Last '/' in @path, or NULL if there is none -- not strrchr(), same
 * reasoning as next_component() above. */
static char *last_slash(const char *path)
{
	const char *p = path;
	const char *found = NULL;

	for (; *p != '\0'; p++)
		if (*p == '/')
			found = p;
	return (char *) found;
}

int dn_policy_init(const char *tree_root_in, const char *rt_root_in)
{
	size_t len;

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

	if (len + sizeof("state/bindings") > PATH_MAX)
		return -ENAMETOOLONG;
	{
		int status = load_bindings(rt_root_in);
		if (status < 0)
			return status;
	}

	/* Fake root and hardlinks are NOT set up here (runtime.md
	 * principle 2: the unit of decision is the individual syscall --
	 * open()/openat() only ever need path mapping, never fake root or
	 * hardlink state, so they shouldn't pay for it or pull in its
	 * dependencies). dn-policy-fakeroot.c and dn-policy-hardlink.c
	 * lazily call dn_policy_fakeroot_init()/dn_policy_hardlink_init()
	 * themselves (via dn_policy_rt_root() below), the first time one
	 * of *their own* functions is actually used. */

	initialized = 1;
	return 0;
}

const char *dn_policy_rt_root(void)
{
	return initialized ? rt_root : NULL;
}

/* Loop guard for both the component walk below and the symlink chain
 * it may chase at each step -- Linux's own SYMLOOP_MAX, not something
 * that needs to be prefix-configurable. */
#define DN_MAX_SYMLINK_DEPTH 40

static int map_and_resolve(const char *guest_path, char *host_path_out, size_t cap,
			int follow_last, int depth);

/* Collapses "." and ".." out of an absolute @path -- needed because a
 * relative symlink target is resolved by plain concatenation against
 * its parent directory (resolve_symlink_chain() below), which can
 * leave a literal ".." in the result; the kernel resolves that fine at
 * every syscall made along the way, but the final string handed back
 * to the caller should be clean, not just functional. A ".." past the
 * root is clamped at the root rather than treated as an error --
 * runtime.md: this is not a security boundary, and a crafted path
 * escaping is someone shooting their own foot, not dn-policy's to
 * police. */
static int normalize_into(const char *path, char *out, size_t cap)
{
	char buf[DN_PATH_BUF];
	const char *stack[64];
	int top = 0;
	char *cursor, *tok;
	size_t len;
	int i;

	if (strlen(path) >= sizeof(buf))
		return -ENAMETOOLONG;
	memcpy(buf, path, strlen(path) + 1);

	cursor = buf;
	tok = next_component(&cursor);
	while (tok != NULL) {
		if (strcmp(tok, ".") == 0) {
			/* skip */
		} else if (strcmp(tok, "..") == 0) {
			if (top > 0)
				top--;
		} else {
			if (top >= (int) (sizeof(stack) / sizeof(stack[0])))
				return -ENAMETOOLONG;
			stack[top++] = tok;
		}
		tok = next_component(&cursor);
	}

	len = 0;
	for (i = 0; i < top; i++) {
		size_t comp_len = strlen(stack[i]);
		if (len + 1 + comp_len + 1 > cap)
			return -ENAMETOOLONG;
		out[len++] = '/';
		memcpy(out + len, stack[i], comp_len);
		len += comp_len;
	}
	if (top == 0) {
		if (cap < 2)
			return -ENAMETOOLONG;
		out[0] = '/';
		len = 1;
	}
	out[len] = '\0';
	return 0;
}

/* The directory part of @path (everything before its last '/'), which
 * for every caller below is non-empty and already absolute, so the
 * only edge case is "/" itself. */
static int dirname_into(const char *path, char *out, size_t cap)
{
	const char *slash = last_slash(path);
	size_t len;

	if (slash == NULL)
		return -EINVAL; /* can't happen for an absolute path */

	len = (slash == path) ? 1 : (size_t) (slash - path); /* keep the leading "/" for "/x" */
	if (len + 1 > cap)
		return -ENAMETOOLONG;
	memcpy(out, path, len);
	out[len] = '\0';
	return 0;
}

/* Resolves whatever @current (a concrete host path, already fully
 * built up to and including its last component) turns out to be: not
 * a symlink (left as is), a relative symlink (resolved against its
 * own parent directory, looping to check the result too), or an
 * absolute symlink (interpreted as a *guest* path per runtime.md and
 * re-resolved from TREE via map_and_resolve(), always following,
 * since this is resolving a symlink's own target, not the caller's
 * final request component). A target or an intermediate component
 * that doesn't exist yet (-ENOENT) is not an error here -- a caller
 * about to create a new file hits this normally -- @current is simply
 * left as its own answer. */
static int resolve_symlink_chain(char *current, size_t cap, int depth)
{
	char target[PATH_MAX];
	char dir[DN_PATH_BUF];
	char resolved[DN_PATH_BUF];
	struct stat st;
	ssize_t n;
	int status;

	for (;;) {
		if (depth++ > DN_MAX_SYMLINK_DEPTH)
			return -ELOOP;

		if (lstat(current, &st) < 0)
			return (errno == ENOENT) ? 0 : -errno;
		if (!S_ISLNK(st.st_mode))
			return 0;

		n = readlink(current, target, sizeof(target) - 1);
		if (n < 0)
			return -errno;
		target[n] = '\0';

		if (target[0] == '/') {
			status = map_and_resolve(target, resolved, sizeof(resolved), 1, depth);
			if (status < 0)
				return status;
		} else {
			status = dirname_into(current, dir, sizeof(dir));
			if (status < 0)
				return status;
			status = path_join(resolved, sizeof(resolved), dir, target);
			if (status < 0)
				return status;
		}

		if (strlen(resolved) + 1 > cap)
			return -ENAMETOOLONG;
		memcpy(current, resolved, strlen(resolved) + 1);
		/* Loop again: @resolved may itself be a symlink.  */
	}
}

/* The component walk: starts at tree_root and appends one guest_path
 * component at a time, resolving any symlink found along the way
 * (resolve_symlink_chain()) before appending the next component on
 * top of it -- so a later component is always appended to where an
 * earlier symlink *really* points, not textually past it. The final
 * component is left alone (not symlink-resolved) when @follow_last is
 * false, matching O_NOFOLLOW/AT_SYMLINK_NOFOLLOW. */
static int map_and_resolve(const char *guest_path, char *host_path_out, size_t cap,
			int follow_last, int depth)
{
	char current[DN_PATH_BUF];
	char rest[PATH_MAX];
	char *cursor;
	char *component;
	char *following;
	int status;

	if (strlen(guest_path) >= sizeof(rest))
		return -ENAMETOOLONG;
	memcpy(rest, guest_path, strlen(guest_path) + 1);

	/* tree_root_len is 0 exactly when tree_root == "/" (see
	 * dn_policy_init()), in which case current must start as "" so
	 * the first appended component becomes "/comp", not "//comp".  */
	memcpy(current, tree_root, tree_root_len);
	current[tree_root_len] = '\0';

	cursor = rest;
	component = next_component(&cursor);
	while (component != NULL) {
		size_t cur_len = strlen(current);
		size_t comp_len = strlen(component);

		if (cur_len + 1 + comp_len + 1 > sizeof(current))
			return -ENAMETOOLONG;
		current[cur_len] = '/';
		memcpy(current + cur_len + 1, component, comp_len + 1);

		following = next_component(&cursor);
		if (following == NULL && !follow_last) {
			component = following;
			break; /* last component, leave it unresolved */
		}

		status = resolve_symlink_chain(current, sizeof(current), depth);
		if (status < 0)
			return status;

		component = following;
	}

	{
		char normalized[DN_PATH_BUF];
		status = normalize_into(current, normalized, sizeof(normalized));
		if (status < 0)
			return status;
		return copy_out(normalized, host_path_out, cap);
	}
}

static int translate_path_common(const char *guest_path, char *host_path_out, size_t cap,
				int follow_last)
{
	if (!initialized)
		return -EINVAL;
	if (guest_path == NULL || guest_path[0] != '/')
		return -EINVAL;

	/* Already a host path (under TREE, or under a row's android side when
	 * that match is at least as specific as any tree-side row) -- "Dịch
	 * không được lặp."  A more specific guest row still wins, so /dev/shm
	 * maps even though /dev is a passthrough row.  */
	{
		size_t gi;
		if (already_host(guest_path, &gi))
			return copy_out(guest_path, host_path_out, cap);
		if (gi != (size_t)-1)
			return map_via_binding(gi, guest_path, host_path_out, cap);
	}

	/* guest_path == "/": tree_root itself, not "tree_root/" (which
	 * has_path_prefix() would still match later, but a bare root is
	 * cleaner with no trailing slash to begin with), and never a
	 * symlink worth resolving.  */
	if (guest_path[1] == '\0')
		return copy_out(tree_root, host_path_out, cap);

	return map_and_resolve(guest_path, host_path_out, cap, follow_last, 0);
}

int dn_policy_redirect_path(const char *guest_path, char *host_path_out, size_t cap)
{
	size_t len;

	if (!initialized)
		return -EINVAL;
	if (guest_path == NULL || guest_path[0] != '/')
		return -EINVAL;

	{
		size_t gi;
		if (already_host(guest_path, &gi))
			return copy_out(guest_path, host_path_out, cap);
		if (gi != (size_t)-1)
			return map_via_binding(gi, guest_path, host_path_out, cap);
	}

	/* TREE == "/" needs no prefix. Otherwise guest paths already begin with
	 * '/', so concatenation needs no separator or normalization. */
	if (tree_root_len == 0)
		return copy_out(guest_path, host_path_out, cap);

	len = tree_root_len + strlen(guest_path);
	if (len + 1 > cap)
		return -ENAMETOOLONG;
	memcpy(host_path_out, tree_root, tree_root_len);
	memcpy(host_path_out + tree_root_len, guest_path, len - tree_root_len + 1);
	return 0;
}

int dn_policy_translate_path(const char *guest_path, char *host_path_out, size_t cap)
{
	return translate_path_common(guest_path, host_path_out, cap, 1);
}

int dn_policy_translate_path_nofollow(const char *guest_path, char *host_path_out, size_t cap)
{
	return translate_path_common(guest_path, host_path_out, cap, 0);
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

	/* Direct bindings, including the default /proc, /sys and /dev entries. */
	{
		size_t i;
	for (i = 0; i < binding_count; i++)
		if (binding_prefix(host_path, bindings[i].host, bindings[i].host_len)) {
			size_t best = i;
			for (size_t j = i + 1; j < binding_count; j++)
				if (bindings[j].host_len > bindings[best].host_len
				    && binding_prefix(host_path, bindings[j].host, bindings[j].host_len))
					best = j;
			if (bindings[best].guest_len + strlen(host_path + bindings[best].host_len) + 1 > cap)
				return -ENAMETOOLONG;
			memcpy(guest_path_out, bindings[best].guest, bindings[best].guest_len);
			memcpy(guest_path_out + bindings[best].guest_len,
			       host_path + bindings[best].host_len,
			       strlen(host_path + bindings[best].host_len) + 1);
			return 0;
		}
	}

	return -ENOENT;
}
