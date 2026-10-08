/* Hand-run check for dn-policy's fake-root layer: DnIdentity, the
 * owner store (whichever backend got probed), and the fake
 * security.* xattr store. Not wired into a build yet. */
#include "dn-policy.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/xattr.h>

static int n_fail = 0;

#define CHECK(cond, what) do { \
	if (!(cond)) { printf("FAIL: %s\n", what); n_fail++; } \
	else printf("ok   %s\n", what); \
} while (0)

int main(void)
{
	char tree[] = "/tmp/dnp-check-tree.XXXXXX";
	char path[PATH_MAX];
	char rt[PATH_MAX];
	int status, fd;
	struct stat st;
	DnIdentity *id1, *id2;
	DnOwnerRecord rec;
	char xattr_in[16] = "capcapcapcapcap";
	char xattr_out[16];

	if (mkdtemp(tree) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(rt, sizeof(rt), "%s/.rt", tree);
	status = mkdir(rt, 0700);
	assert(status == 0);

	status = dn_policy_init(tree, rt);
	CHECK(status == 0, "dn_policy_init");

	/* DnIdentity: two handles never share state.  */
	id1 = dn_policy_identity_new();
	id2 = dn_policy_identity_new();
	CHECK(id1 != NULL && id2 != NULL, "identity_new");
	CHECK(dn_policy_fake_getuid(id1) == 0, "fresh identity starts at uid 0");
	dn_policy_fake_setuid(id1, 105); /* e.g. "_apt" */
	CHECK(dn_policy_fake_getuid(id1) == 105, "setuid sticks on id1");
	CHECK(dn_policy_fake_getuid(id2) == 0, "id2 untouched by id1's setuid");
	dn_policy_identity_free(id1);
	dn_policy_identity_free(id2);

	/* Owner store: a real file, no record yet -> fake_stat reports 0/0.  */
	snprintf(path, sizeof(path), "%s/somefile", tree);
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	assert(fd >= 0);
	close(fd);

	status = stat(path, &st);
	assert(status == 0);
	dn_policy_fake_stat(path, &st);
	CHECK(st.st_uid == 0 && st.st_gid == 0, "no owner record -> uid 0, gid 0");

	rec.uid = 1000;
	rec.gid = 1000;
	rec.mode_bits = 04000; /* setuid bit */
	rec.rdev = 0;
	status = dn_policy_owner_set(path, st.st_dev, st.st_ino, &rec);
	CHECK(status == 0, "owner_set");

	status = stat(path, &st); /* mode_bits may differ in real st_mode now */
	assert(status == 0);
	dn_policy_fake_stat(path, &st);
	CHECK(st.st_uid == 1000 && st.st_gid == 1000, "owner_set changes fake_stat's uid/gid");
	CHECK((st.st_mode & 04000) != 0, "owner_set's setuid bit shows up in fake_stat");

	/* owner_forget's contract is "called when the file's last link is
	 * removed" (dn-policy.h) -- the xattr backend is correctly a
	 * no-op on a still-living file (the record lives ON the file),
	 * so this only means something once that link is actually gone.
	 * Unlink for real, forget, then recreate at the same path: a
	 * fresh inode under the xattr backend simply never had the
	 * xattr; under the DB backend, forget already dropped the
	 * (dev, ino) key, and even an inode-number reuse would still
	 * find nothing since forget cleared it. Either way the new file
	 * must come back with no record. */
	status = unlink(path);
	assert(status == 0);
	status = dn_policy_owner_forget(path, st.st_dev, st.st_ino);
	CHECK(status == 0, "owner_forget, after the real unlink");

	fd = open(path, O_CREAT | O_WRONLY, 0644);
	assert(fd >= 0);
	close(fd);
	status = stat(path, &st);
	assert(status == 0);
	dn_policy_fake_stat(path, &st);
	CHECK(st.st_uid == 0 && st.st_gid == 0,
	      "a file recreated after unlink+forget carries no leftover owner record");

	/* DB backend only: the slot forget() just freed above must be
	 * reusable by a later set() on a *different* file, not just
	 * appended past (db_find_free_slot()'s one job). Exercise it with
	 * a second file; the two owner records must not cross-contaminate
	 * either way. */
	{
		char path2[PATH_MAX];
		struct stat st1, st2;
		DnOwnerRecord rec2 = { .uid = 2000, .gid = 2000, .mode_bits = 0, .rdev = 0 };

		snprintf(path2, sizeof(path2), "%s/otherfile", tree);
		fd = open(path2, O_CREAT | O_WRONLY, 0644);
		assert(fd >= 0);
		close(fd);

		status = stat(path, &st1);
		assert(status == 0);
		status = stat(path2, &st2);
		assert(status == 0);

		status = dn_policy_owner_set(path, st1.st_dev, st1.st_ino, &rec);
		CHECK(status == 0, "re-set path's record (reuses the freed slot under DB)");
		status = dn_policy_owner_set(path2, st2.st_dev, st2.st_ino, &rec2);
		CHECK(status == 0, "set a second, different file's record");

		status = stat(path, &st1);
		dn_policy_fake_stat(path, &st1);
		status = stat(path2, &st2);
		dn_policy_fake_stat(path2, &st2);
		CHECK(st1.st_uid == 1000 && st2.st_uid == 2000,
		      "the two files' records did not cross-contaminate");

		unlink(path2);
	}

	/* Fake security.* xattr, round-tripped.  */
	CHECK(dn_policy_is_fake_xattr("security.capability") == 1, "security.* recognized");
	CHECK(dn_policy_is_fake_xattr("user.dn.owner") == 0, "non-security.* not treated as fake");

	status = dn_policy_fake_setxattr(path, st.st_dev, st.st_ino,
					  "security.capability", xattr_in, sizeof(xattr_in));
	CHECK(status == 0, "fake_setxattr");

	status = dn_policy_fake_getxattr(path, st.st_dev, st.st_ino,
					  "security.capability", xattr_out, sizeof(xattr_out));
	CHECK(status == (int) sizeof(xattr_in) && memcmp(xattr_in, xattr_out, sizeof(xattr_in)) == 0,
	      "fake_getxattr round-trips what fake_setxattr wrote");

	/* The real xattr was never touched -- it should not exist.  */
	status = (int) getxattr(path, "security.capability", xattr_out, sizeof(xattr_out));
	CHECK(status < 0 && errno == ENODATA, "the real security.* xattr was never written");

	status = dn_policy_fake_removexattr(path, st.st_dev, st.st_ino, "security.capability");
	CHECK(status == 0, "fake_removexattr");
	status = dn_policy_fake_getxattr(path, st.st_dev, st.st_ino,
					  "security.capability", xattr_out, sizeof(xattr_out));
	CHECK(status == -ENOENT, "fake_getxattr -ENOENT after fake_removexattr");

	if (n_fail == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) FAILED\n", n_fail);

	return n_fail == 0 ? 0 : 1;
}
