/* Hand-run check for dn-policy's link2symlink hardlinks. Not wired
 * into a build yet. */
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

static void write_all(const char *path, const char *content)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, content, strlen(content)) == (ssize_t) strlen(content));
	close(fd);
}

static int read_all(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

int main(void)
{
	/* Deliberately not under /tmp: this repo's *existing* tracer
	 * (src/tracer/path/temp.c) special-cases /tmp, which broke
	 * rename() across it during development of this check -- a
	 * quirk of the currently-running old overlay, not of dn-policy.
	 * Right under the real home dir sidesteps that and is closer to
	 * where TREE actually lives anyway. */
	char tree[] = "/data/data/com.termux/files/home/.dnp-check-hl.XXXXXX";
	setbuf(stdout, NULL);
	char rt[PATH_MAX];
	char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX];
	int status;
	struct stat sa, sb, sc;
	char buf[64];

	if (mkdtemp(tree) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(rt, sizeof(rt), "%s/.rt", tree);
	status = mkdir(rt, 0700);
	assert(status == 0);
	status = dn_policy_init(tree, rt);
	assert(status == 0);

	snprintf(a, sizeof(a), "%s/a", tree);
	snprintf(b, sizeof(b), "%s/b", tree);
	snprintf(c, sizeof(c), "%s/c", tree);
	write_all(a, "hello hardlink");

	/* First link: a (ordinary file) + b.  */
	status = dn_policy_link(a, b);
	CHECK(status == 0, "dn_policy_link(a, b)");

	status = lstat(a, &sa);
	assert(status == 0);
	CHECK(S_ISLNK(sa.st_mode), "'a' is now really a symlink on disk (lstat sees through fake_stat)");

	status = stat(a, &sa);
	assert(status == 0);
	dn_policy_fake_stat(a, &sa);
	CHECK(S_ISREG(sa.st_mode), "fake_stat(a) reports a regular file, not a symlink");
	CHECK(sa.st_nlink == 2, "fake_stat(a) reports nlink == 2 after one link()");

	/* lstat must agree with stat for a managed name (real hardlinks
	 * never differ between the two).  */
	status = lstat(a, &sa);
	assert(status == 0);
	dn_policy_fake_stat(a, &sa);
	CHECK(S_ISREG(sa.st_mode) && sa.st_nlink == 2,
	      "fake_stat(lstat(a)) agrees with fake_stat(stat(a))");

	status = stat(b, &sb);
	assert(status == 0);
	dn_policy_fake_stat(b, &sb);
	CHECK(sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino,
	      "'a' and 'b' report the same (dev, ino), like a real hardlink");

	status = read_all(b, buf, sizeof(buf));
	CHECK(status == 0 && strcmp(buf, "hello hardlink") == 0,
	      "'b' reads back 'a''s original content");

	/* A second link, added to an already-managed name.  */
	status = dn_policy_link(b, c);
	CHECK(status == 0, "dn_policy_link(b, c) -- b is already managed");

	status = stat(c, &sc);
	assert(status == 0);
	dn_policy_fake_stat(c, &sc);
	CHECK(sc.st_dev == sa.st_dev && sc.st_ino == sa.st_ino, "'c' shares the same identity too");

	status = stat(a, &sa);
	dn_policy_fake_stat(a, &sa);
	CHECK(sa.st_nlink == 3, "nlink is 3 with all of a, b, c live");

	/* Unlinking one name drops the count but the other two still
	 * read correctly.  */
	status = dn_policy_unlink(c);
	CHECK(status == 0, "dn_policy_unlink(c) bookkeeping");
	status = unlink(c); /* the real removal, as documented order */
	assert(status == 0);

	status = stat(a, &sa);
	dn_policy_fake_stat(a, &sa);
	CHECK(sa.st_nlink == 2, "nlink drops to 2 after unlinking c");

	status = read_all(a, buf, sizeof(buf));
	CHECK(status == 0 && strcmp(buf, "hello hardlink") == 0, "'a' still reads correctly");
	status = read_all(b, buf, sizeof(buf));
	CHECK(status == 0 && strcmp(buf, "hello hardlink") == 0, "'b' still reads correctly");

	/* Unlinking down to the last name removes the hidden file.  */
	status = dn_policy_unlink(a);
	CHECK(status == 0, "dn_policy_unlink(a) bookkeeping");
	status = unlink(a);
	assert(status == 0);

	status = dn_policy_unlink(b);
	CHECK(status == 0, "dn_policy_unlink(b) bookkeeping (last name)");
	status = unlink(b);
	assert(status == 0);

	/* An ordinary file is untouched by any of this.  */
	snprintf(a, sizeof(a), "%s/ordinary", tree);
	write_all(a, "just a file");
	status = dn_policy_unlink(a); /* not managed: should be a no-op */
	CHECK(status == 0, "dn_policy_unlink on an ordinary (unmanaged) file is a no-op");
	status = access(a, F_OK);
	CHECK(status == 0, "...and does not delete the ordinary file");

	status = stat(a, &sa);
	dn_policy_fake_stat(a, &sa);
	CHECK(S_ISREG(sa.st_mode) && sa.st_nlink == 1,
	      "fake_stat on an ordinary file is unaffected by the hardlink layer");

	if (n_fail == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) FAILED\n", n_fail);

	return n_fail == 0 ? 0 : 1;
}
