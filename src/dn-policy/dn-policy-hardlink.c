/* dn-policy -- hardlinks via link2symlink. See dn-policy.h for the
 * calling convention and docs/spec/runtime.md, "dn-policy" >
 * "Hardlinks" for the rules implemented here.
 *
 * A hardlinked name is really a symlink to a hidden regular file under
 * "<rt_root>/state/links/<id>". <id> is "<dev>-<ino>" of the original
 * file at the moment it was first linked (the only point its real
 * identity is still around to name the hidden file with) -- a
 * near-certain-in-practice but not airtight key, since a (dev, ino)
 * pair can be reused once the original file is gone; accepted as a
 * known limitation of this increment rather than adding a persisted
 * counter for one more bit of robustness. "<rt_root>/state/links.db" is
 * the refcount for each <id>, a flat file of fixed-size records
 * guarded by flock() -- the same shape as dn-policy-fakeroot.c's
 * owners.db, kept as a separate file (and separate, if near-identical,
 * helper functions here) rather than sharing code with it, since the
 * two stores have no reason to ever be read or locked together.
 */

#include "dn-policy.h"
#include "dn-policy-internal.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdio.h>

static char links_dir[PATH_MAX];
static char links_db_path[PATH_MAX];
static int ready;

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

int dn_policy_hardlink_init(const char *rt_root)
{
	int status;

	status = snprintf(links_dir, sizeof(links_dir), "%s/state/links", rt_root);
	if (status < 0 || (size_t) status >= sizeof(links_dir))
		return -ENAMETOOLONG;
	status = ensure_dir(links_dir);
	if (status < 0)
		return status;

	status = snprintf(links_db_path, sizeof(links_db_path), "%s/state/links.db", rt_root);
	if (status < 0 || (size_t) status >= sizeof(links_db_path))
		return -ENAMETOOLONG;

	ready = 1;
	return 0;
}

/* ---- The refcount DB: fixed-size records, scanned linearly -------- */

#define DN_LINK_ID_CAP 48 /* "<dev>-<ino>" as decimal, comfortably fits */

typedef struct {
	char     id[DN_LINK_ID_CAP];
	uint32_t refcount;
	uint8_t  valid;
} LinkRecord;

static int open_locked(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -errno;
	if (flock(fd, LOCK_EX) < 0) {
		int err = -errno;
		close(fd);
		return err;
	}
	return fd;
}

static int db_find(int fd, const char *id, LinkRecord *out, off_t *offset_out)
{
	LinkRecord rec;
	off_t offset = 0;
	ssize_t n;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;

	for (;;) {
		n = read(fd, &rec, sizeof(rec));
		if (n == 0)
			return -ENOENT;
		if (n != (ssize_t) sizeof(rec))
			return -EIO;
		if (rec.valid && strncmp(rec.id, id, sizeof(rec.id)) == 0) {
			if (out != NULL)
				*out = rec;
			if (offset_out != NULL)
				*offset_out = offset;
			return 0;
		}
		offset += (off_t) sizeof(rec);
	}
}

static int db_find_free_slot(int fd, off_t *offset_out)
{
	LinkRecord rec;
	off_t offset = 0;
	ssize_t n;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;

	for (;;) {
		n = read(fd, &rec, sizeof(rec));
		if (n == 0)
			return -ENOENT;
		if (n != (ssize_t) sizeof(rec))
			return -EIO;
		if (!rec.valid) {
			*offset_out = offset;
			return 0;
		}
		offset += (off_t) sizeof(rec);
	}
}

static int db_write_at(int fd, off_t offset, const LinkRecord *rec)
{
	return (pwrite(fd, rec, sizeof(*rec), offset) == (ssize_t) sizeof(*rec)) ? 0 : -EIO;
}

static int db_set_refcount(const char *id, uint32_t refcount)
{
	LinkRecord rec, existing;
	off_t offset;
	int fd, status;

	fd = open_locked(links_db_path);
	if (fd < 0)
		return fd;

	memset(&rec, 0, sizeof(rec));
	strncpy(rec.id, id, sizeof(rec.id) - 1);
	rec.refcount = refcount;
	rec.valid = 1;

	status = db_find(fd, id, &existing, &offset);
	if (status == 0) {
		status = db_write_at(fd, offset, &rec);
	} else if (status == -ENOENT) {
		status = db_find_free_slot(fd, &offset);
		if (status == 0) {
			status = db_write_at(fd, offset, &rec);
		} else {
			off_t end = lseek(fd, 0, SEEK_END);
			status = (end < 0) ? -errno : db_write_at(fd, end, &rec);
		}
	}

	close(fd);
	return status;
}

/* Returns 0 with *@out set, or -ENOENT if @id has no record. */
static int db_get_refcount(const char *id, uint32_t *out)
{
	LinkRecord rec;
	int fd, status;

	fd = open_locked(links_db_path);
	if (fd < 0)
		return fd;

	status = db_find(fd, id, &rec, NULL);
	close(fd);
	if (status < 0)
		return status;

	*out = rec.refcount;
	return 0;
}

static int db_forget(const char *id)
{
	LinkRecord rec;
	off_t offset;
	int fd, status;

	fd = open_locked(links_db_path);
	if (fd < 0)
		return fd;

	status = db_find(fd, id, &rec, &offset);
	if (status == 0) {
		rec.valid = 0;
		status = db_write_at(fd, offset, &rec);
	} else if (status == -ENOENT) {
		status = 0;
	}

	close(fd);
	return status;
}

/* ---- Naming: the hidden file's path, and its <id> -------------------- */

static void make_id(dev_t dev, ino_t ino, char *out, size_t cap)
{
	snprintf(out, cap, "%llu-%llu", (unsigned long long) dev, (unsigned long long) ino);
}

static int hidden_path_for_id(const char *id, char *out, size_t cap)
{
	int status = snprintf(out, cap, "%s/%s", links_dir, id);
	if (status < 0 || (size_t) status >= cap)
		return -ENAMETOOLONG;
	return 0;
}

/* If @host_path is a symlink into links_dir, fills *@id (and, when
 * non-NULL, *@hidden_path) and returns 1. Returns 0 if @host_path is
 * not a symlink, or is a symlink elsewhere (an ordinary one, not ours).
 * A negative errno only for an unexpected failure, never for "it's
 * just not a managed name". */
static int read_managed_target(const char *host_path, char *id_out, size_t id_cap,
		char *hidden_path_out, size_t hidden_cap)
{
	char target[PATH_MAX];
	ssize_t n;
	size_t dir_len = strlen(links_dir);
	const char *base;

	n = readlink(host_path, target, sizeof(target) - 1);
	if (n < 0)
		return (errno == EINVAL) ? 0 : 0; /* EINVAL: not a symlink at all */
	target[n] = '\0';

	if (strncmp(target, links_dir, dir_len) != 0 || target[dir_len] != '/')
		return 0;

	base = target + dir_len + 1;
	if (strlen(base) >= id_cap)
		return -ENAMETOOLONG;
	memcpy(id_out, base, strlen(base) + 1);

	if (hidden_path_out != NULL) {
		if ((size_t) n >= hidden_cap)
			return -ENAMETOOLONG;
		memcpy(hidden_path_out, target, (size_t) n + 1);
	}
	return 1;
}

/* ---- Public API ------------------------------------------------------ */

int dn_policy_link(const char *existing_host_path, const char *new_host_path)
{
	char id[DN_LINK_ID_CAP];
	char hidden_path[PATH_MAX];
	struct stat st;
	uint32_t refcount;
	int status;

	if (!ready)
		return -EINVAL;

	status = read_managed_target(existing_host_path, id, sizeof(id), hidden_path, sizeof(hidden_path));
	if (status < 0)
		return status;

	if (status == 1) {
		/* Already managed: just add another symlink and bump the
		 * count. */
		if (symlink(hidden_path, new_host_path) < 0)
			return -errno;
		status = db_get_refcount(id, &refcount);
		if (status < 0)
			return status;
		return db_set_refcount(id, refcount + 1);
	}

	/* First time: @existing_host_path must be an ordinary regular
	 * file right now. */
	if (lstat(existing_host_path, &st) < 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EPERM;

	make_id(st.st_dev, st.st_ino, id, sizeof(id));
	status = hidden_path_for_id(id, hidden_path, sizeof(hidden_path));
	if (status < 0)
		return status;

	/* A stale hidden file from a crashed earlier run, or (vanishingly
	 * unlikely) a (dev, ino) collision -- refuse rather than silently
	 * overwrite something that might still be in use. */
	if (access(hidden_path, F_OK) == 0)
		return -EEXIST;

	if (rename(existing_host_path, hidden_path) < 0)
		return -errno;
	if (symlink(hidden_path, existing_host_path) < 0) {
		/* Best-effort rollback so the name isn't left missing. */
		rename(hidden_path, existing_host_path);
		return -errno;
	}
	if (symlink(hidden_path, new_host_path) < 0) {
		int err = -errno;
		unlink(existing_host_path);
		rename(hidden_path, existing_host_path);
		return err;
	}

	return db_set_refcount(id, 2);
}

int dn_policy_unlink(const char *host_path)
{
	char id[DN_LINK_ID_CAP];
	char hidden_path[PATH_MAX];
	uint32_t refcount;
	int status;

	if (!ready)
		return -EINVAL;

	status = read_managed_target(host_path, id, sizeof(id), hidden_path, sizeof(hidden_path));
	if (status <= 0)
		return (status < 0) ? status : 0; /* not managed: nothing to do */

	status = db_get_refcount(id, &refcount);
	if (status < 0)
		return 0; /* no record: stale, nothing sensible to do */

	if (refcount <= 1) {
		if (unlink(hidden_path) < 0 && errno != ENOENT)
			return -errno;
		return db_forget(id);
	}

	return db_set_refcount(id, refcount - 1);
}

int dn_policy_hardlink_fixup_stat(const char *host_path, struct stat *st)
{
	char id[DN_LINK_ID_CAP];
	char hidden_path[PATH_MAX];
	uint32_t refcount;
	int status;

	if (!ready)
		return -EINVAL;

	status = read_managed_target(host_path, id, sizeof(id), hidden_path, sizeof(hidden_path));
	if (status <= 0)
		return status;

	/* Replace *st wholesale: an lstat() on a real hardlink never
	 * differs from stat(), so a managed name must not either,
	 * regardless of which one the caller actually issued. */
	if (stat(hidden_path, st) < 0)
		return -errno;

	status = db_get_refcount(id, &refcount);
	st->st_nlink = (status == 0) ? refcount : 1;

	return 1;
}
