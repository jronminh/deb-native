/* Hand-run check for dn-policy's absolute in-tree symlink resolution.
 * Not wired into a build yet. Deliberately not under /tmp -- see the
 * note in .check_hardlink.c (this repo's existing tracer special-cases
 * /tmp and that broke an earlier check). */
#include "dn-policy.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

static int n_fail = 0;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("FAIL: %s\n", what); n_fail++; } \
	else printf("ok   %s\n", what); \
} while (0)

static void mkdir_p(const char *path)
{
	int status = mkdir(path, 0755);
	assert(status == 0 || errno == EEXIST);
}

static void write_all(const char *path, const char *content)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, content, strlen(content)) == (ssize_t) strlen(content));
	close(fd);
}

int main(void)
{
	char tree[] = "/data/data/com.termux/files/home/.dnp-check-sym.XXXXXX";
	char rt[PATH_MAX];
	char buf[PATH_MAX];
	char path[PATH_MAX];
	int status;

	setbuf(stdout, NULL);

	if (mkdtemp(tree) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(rt, sizeof(rt), "%s/.rt", tree);
	mkdir_p(rt);
	status = dn_policy_init(tree, rt);
	assert(status == 0);

	/* Real tree layout:
	 *   real/standard.flf          -- the actual target file
	 *   etc/alternatives/foo -> /real/standard.flf     (absolute, in-tree)
	 *   etc/rellink -> ../real/standard.flf             (relative)
	 *   etc/chain -> /etc/alternatives/foo              (absolute -> absolute)
	 */
	snprintf(buf, sizeof(buf), "%s/real", tree);
	mkdir_p(buf);
	snprintf(path, sizeof(path), "%s/real/standard.flf", tree);
	write_all(path, "banner-data");

	snprintf(buf, sizeof(buf), "%s/etc", tree);
	mkdir_p(buf);
	snprintf(buf, sizeof(buf), "%s/etc/alternatives", tree);
	mkdir_p(buf);

	snprintf(buf, sizeof(buf), "%s/etc/alternatives/foo", tree);
	status = symlink("/real/standard.flf", buf);
	assert(status == 0);

	snprintf(buf, sizeof(buf), "%s/etc/relink", tree);
	status = symlink("../real/standard.flf", buf);
	assert(status == 0);

	snprintf(buf, sizeof(buf), "%s/etc/chain", tree);
	status = symlink("/etc/alternatives/foo", buf);
	assert(status == 0);

	/* Absolute in-tree symlink: /etc/alternatives/foo -> /real/standard.flf,
	 * resolved against TREE, not against the real Android root.  */
	status = dn_policy_translate_path("/etc/alternatives/foo", buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/real/standard.flf", tree);
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "absolute in-tree symlink resolves into TREE, not the real root");

	/* Relative symlink: resolved against its own parent directory,
	 * same real file.  */
	status = dn_policy_translate_path("/etc/relink", buf, sizeof(buf));
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "relative symlink resolves relative to its own directory");

	/* Chain: absolute symlink pointing at another absolute symlink.  */
	status = dn_policy_translate_path("/etc/chain", buf, sizeof(buf));
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "a chain of absolute symlinks resolves all the way through");

	/* A path through the symlink, not just the symlink itself:
	 * /etc/alternatives/foo doesn't have further components after
	 * it here, but resolving a path *past* a chain is what matters
	 * for a real package layout (e.g. python3.X/) -- cover the one
	 * component after it as well. */
	snprintf(buf, sizeof(buf), "%s/real/sub", tree);
	mkdir_p(buf);
	write_all((snprintf(path, sizeof(path), "%s/real/sub/deep", tree), path), "deep-data");
	snprintf(buf, sizeof(buf), "%s/etc/alt2", tree);
	status = symlink("/real/sub", buf);
	assert(status == 0);
	status = dn_policy_translate_path("/etc/alt2/deep", buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/real/sub/deep", tree);
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "a component appended after a resolved symlink lands in the right place");

	/* O_NOFOLLOW / AT_SYMLINK_NOFOLLOW: the final component itself is
	 * left as the symlink's own (translated) path, not its target. */
	status = dn_policy_translate_path_nofollow("/etc/alternatives/foo", buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/etc/alternatives/foo", tree);
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "translate_path_nofollow leaves the final symlink component itself");

	/* But an intermediate symlink on the way to the final component
	 * is still resolved even under _nofollow -- only the *last*
	 * component is left alone. */
	snprintf(buf, sizeof(buf), "%s/etc/alt2x", tree);
	status = symlink("/real/sub", buf);
	assert(status == 0);
	status = dn_policy_translate_path_nofollow("/etc/alt2x/deep", buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/real/sub/deep", tree);
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "_nofollow still resolves an intermediate symlink, just not the final component");

	/* A path that doesn't exist yet (about to be created) is not an
	 * error -- just the nominal mapped path, no symlink to chase. */
	status = dn_policy_translate_path("/etc/does-not-exist-yet", buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/etc/does-not-exist-yet", tree);
	CHECK(status == 0 && strcmp(buf, path) == 0,
	      "a nonexistent target path translates to its nominal location, no error");

	/* A self-referential symlink hits the loop guard rather than
	 * spinning forever. */
	snprintf(buf, sizeof(buf), "%s/etc/loopy", tree);
	status = symlink("/etc/loopy", buf);
	assert(status == 0);
	status = dn_policy_translate_path("/etc/loopy", buf, sizeof(buf));
	CHECK(status == -ELOOP, "a self-referential symlink returns -ELOOP, not an infinite loop");

	if (n_fail == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) FAILED\n", n_fail);

	return n_fail == 0 ? 0 : 1;
}
