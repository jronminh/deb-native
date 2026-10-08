/* dn-policy -- fake root. See dn-policy.h for the calling convention
 * and docs/spec/runtime.md, "dn-policy" > "Fake root" for the rules
 * implemented here.
 *
 * Two owner-store backends, chosen once at dn_policy_init() by probing
 * the device (runtime.md: "phải thử trên máy"):
 *
 *   - xattr: the record lives on the file itself, as the "user.dn.owner"
 *     xattr, serialized as a fixed-size blob. No key needed -- the file
 *     carries its own record and it survives a rename for free.
 *   - db: a flat file of fixed-size records at "<rt_root>/state/owners.db",
 *     keyed by (st_dev, st_ino), guarded by an flock() for the whole
 *     read-modify-write (multiple *processes* write this, not just
 *     threads -- a plain in-process mutex is not enough).
 *
 * security.* xattrs are never attempted for real (that real write is
 * what's failing to begin with) -- they always go into a second DB file,
 * "<rt_root>/state/xattrs.db", regardless of which backend above was
 * chosen for ownership.
 *
 * This file does no path translation of its own; every @host_path it
 * receives is assumed already translated by the caller.
 */

#include "dn-policy.h"
#include "dn-policy-internal.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define DN_OWNER_XATTR "user.dn.owner"

static char state_dir[PATH_MAX];
static char owners_db_path[PATH_MAX];
static char xattrs_db_path[PATH_MAX];
static int backend_is_xattr;
static int state_ready;

/* ---- DnIdentity (per traced program, never a dn-policy global) ---- */

struct DnIdentity {
	uid_t uid;
	gid_t gid;
};

DnIdentity *dn_policy_identity_new(void)
{
	DnIdentity *id = malloc(sizeof(*id));
	if (id == NULL)
		return NULL;
	id->uid = 0;
	id->gid = 0;
	return id;
}

void dn_policy_identity_free(DnIdentity *id)
{
	free(id);
}

uid_t dn_policy_fake_getuid(const DnIdentity *id)
{
	return id->uid;
}

gid_t dn_policy_fake_getgid(const DnIdentity *id)
{
	return id->gid;
}

void dn_policy_fake_setuid(DnIdentity *id, uid_t uid)
{
	id->uid = uid;
}

void dn_policy_fake_setgid(DnIdentity *id, gid_t gid)
{
	id->gid = gid;
}

/* ---- Setup: probe once, from dn_policy_init() ---------------------- */

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

/* Returns 1 if the device lets us write/read/remove a user.* xattr on a
 * real file under @rt_root, 0 if not, or a negative errno on an
 * unexpected failure setting the probe up (not on the xattr calls
 * themselves -- failing *those* just means "0, use the DB instead"). */
static int probe_user_xattr(const char *rt_root)
{
	char probe_path[PATH_MAX];
	int fd;
	int status;
	char value[8];

	status = snprintf(probe_path, sizeof(probe_path), "%s/.xattr-probe", state_dir);
	if (status < 0 || (size_t) status >= sizeof(probe_path))
		return -ENAMETOOLONG;
	(void) rt_root;

	fd = open(probe_path, O_CREAT | O_WRONLY, 0600);
	if (fd < 0)
		return -errno;
	close(fd);

	if (setxattr(probe_path, "user.dn.probe", "1", 1, 0) < 0) {
		unlink(probe_path);
		return 0;
	}
	if (getxattr(probe_path, "user.dn.probe", value, sizeof(value)) != 1) {
		unlink(probe_path);
		return 0;
	}
	removexattr(probe_path, "user.dn.probe");
	unlink(probe_path);
	return 1;
}

/* Not called from dn_policy_init() -- see dn_policy_rt_root()'s comment
 * in dn-policy-internal.h. Called lazily, on demand, by ensure_ready()
 * below; idempotent so every public entry point in this file can call
 * it unconditionally without re-probing once it has already succeeded. */
int dn_policy_fakeroot_init(const char *rt_root)
{
	int status;

	if (state_ready)
		return 0;

	status = snprintf(state_dir, sizeof(state_dir), "%s/state", rt_root);
	if (status < 0 || (size_t) status >= sizeof(state_dir))
		return -ENAMETOOLONG;

	status = ensure_dir(state_dir);
	if (status < 0)
		return status;

	status = snprintf(owners_db_path, sizeof(owners_db_path), "%s/owners.db", state_dir);
	if (status < 0 || (size_t) status >= sizeof(owners_db_path))
		return -ENAMETOOLONG;
	status = snprintf(xattrs_db_path, sizeof(xattrs_db_path), "%s/xattrs.db", state_dir);
	if (status < 0 || (size_t) status >= sizeof(xattrs_db_path))
		return -ENAMETOOLONG;

	/* Escape hatch for testing/diagnosis, same convention as DN_ID
	 * (syscall/exit.c): skips the probe entirely when set. "db" is
	 * also how to exercise the DB backend's code path on a device
	 * whose app-data partition does support user.* xattrs (every
	 * such partition seen so far does) -- there is otherwise no way
	 * to reach it short of a filesystem that lacks xattr support. */
	{
		const char *forced = getenv("DN_POLICY_OWNER_BACKEND");
		if (forced != NULL && strcmp(forced, "db") == 0) {
			backend_is_xattr = 0;
			state_ready = 1;
			return 0;
		}
		if (forced != NULL && strcmp(forced, "xattr") == 0) {
			backend_is_xattr = 1;
			state_ready = 1;
			return 0;
		}
	}

	status = probe_user_xattr(rt_root);
	if (status < 0)
		return status;
	backend_is_xattr = status;

	state_ready = 1;
	return 0;
}

/* ---- Lazy init, for every public entry point below ----------------- */

static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;

/* Called at the top of every dn_policy_owner_*()/dn_policy_fake_*()
 * function. Cheap once ready (one branch, no lock) -- the lock only
 * matters for the first call(s), possibly racing from multiple
 * threads. Plain pthread, not glibc's __libc_lock: this file is built
 * standalone (dn-trace) as well as folded into dn-glibc. */
static int ensure_ready(void)
{
	const char *rt_root;
	int status;

	if (state_ready)
		return 0;

	pthread_mutex_lock(&ready_lock);
	if (state_ready) {
		pthread_mutex_unlock(&ready_lock);
		return 0;
	}

	rt_root = dn_policy_rt_root();
	status = (rt_root == NULL) ? -EINVAL : dn_policy_fakeroot_init(rt_root);

	pthread_mutex_unlock(&ready_lock);
	return status;
}

/* ---- The DB backend: fixed-size records, scanned linearly --------- */

typedef struct {
	uint64_t dev;
	uint64_t ino;
	uint32_t uid;
	uint32_t gid;
	uint32_t mode_bits;
	uint32_t rdev;
	uint8_t  valid;
} DbRecord;

/* Opens @path, takes an exclusive flock() for the lifetime of the fd
 * (released on close()), creating the file if absent. Returns the fd,
 * or a negative errno. */
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

static int db_find(int fd, uint64_t dev, uint64_t ino, DbRecord *out, off_t *offset_out)
{
	DbRecord rec;
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
		if (rec.valid && rec.dev == dev && rec.ino == ino) {
			if (out != NULL)
				*out = rec;
			if (offset_out != NULL)
				*offset_out = offset;
			return 0;
		}
		offset += (off_t) sizeof(rec);
	}
}

static int db_write_at(int fd, off_t offset, const DbRecord *rec)
{
	ssize_t n = pwrite(fd, rec, sizeof(*rec), offset);
	if (n != (ssize_t) sizeof(*rec))
		return -EIO;
	return 0;
}

static int db_append(int fd, const DbRecord *rec)
{
	off_t end = lseek(fd, 0, SEEK_END);
	if (end < 0)
		return -errno;
	return db_write_at(fd, end, rec);
}

/* Finds the first invalid (freed) slot, for reuse by a later set().
 * Returns 0 with *@offset_out set, or -ENOENT if every record so far is
 * valid (caller should append instead). */
static int db_find_free_slot(int fd, off_t *offset_out)
{
	DbRecord rec;
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

/* ---- Owner store, dispatched to whichever backend was probed ------ */

static void record_to_disk(const DnOwnerRecord *in, uint64_t dev, uint64_t ino, DbRecord *out)
{
	out->dev = dev;
	out->ino = ino;
	out->uid = in->uid;
	out->gid = in->gid;
	out->mode_bits = (uint32_t) in->mode_bits;
	out->rdev = (uint32_t) in->rdev;
	out->valid = 1;
}

static void disk_to_record(const DbRecord *in, DnOwnerRecord *out)
{
	out->uid = in->uid;
	out->gid = in->gid;
	out->mode_bits = in->mode_bits;
	out->rdev = in->rdev;
}

static int owner_get_xattr(const char *host_path, DnOwnerRecord *out)
{
	ssize_t n = getxattr(host_path, DN_OWNER_XATTR, out, sizeof(*out));
	if (n < 0)
		return (errno == ENODATA) ? -ENOENT : -errno; /* Linux has no ENOATTR */
	if (n != (ssize_t) sizeof(*out))
		return -EIO;
	return 0;
}

static int owner_set_xattr(const char *host_path, const DnOwnerRecord *record)
{
	if (setxattr(host_path, DN_OWNER_XATTR, record, sizeof(*record), 0) < 0)
		return -errno;
	return 0;
}

static int owner_get_db(dev_t dev, ino_t ino, DnOwnerRecord *out)
{
	DbRecord rec;
	int fd, status;

	fd = open_locked(owners_db_path);
	if (fd < 0)
		return fd;

	status = db_find(fd, dev, ino, &rec, NULL);
	close(fd); /* also releases the flock() */
	if (status < 0)
		return status;

	disk_to_record(&rec, out);
	return 0;
}

static int owner_set_db(dev_t dev, ino_t ino, const DnOwnerRecord *record)
{
	DbRecord rec, existing;
	off_t offset;
	int fd, status;

	fd = open_locked(owners_db_path);
	if (fd < 0)
		return fd;

	record_to_disk(record, dev, ino, &rec);

	status = db_find(fd, dev, ino, &existing, &offset);
	if (status == 0) {
		status = db_write_at(fd, offset, &rec);
	} else if (status == -ENOENT) {
		status = db_find_free_slot(fd, &offset);
		status = (status == 0) ? db_write_at(fd, offset, &rec) : db_append(fd, &rec);
	}

	close(fd);
	return status;
}

static int owner_forget_db(dev_t dev, ino_t ino)
{
	off_t offset;
	int fd, status;
	DbRecord rec;

	fd = open_locked(owners_db_path);
	if (fd < 0)
		return fd;

	status = db_find(fd, dev, ino, &rec, &offset);
	if (status == 0) {
		rec.valid = 0;
		status = db_write_at(fd, offset, &rec);
	} else if (status == -ENOENT) {
		status = 0; /* nothing to forget is not an error */
	}

	close(fd);
	return status;
}

int dn_policy_owner_get(const char *host_path, dev_t dev, ino_t ino, DnOwnerRecord *out)
{
	int status = ensure_ready();
	if (status < 0)
		return status;
	return backend_is_xattr ? owner_get_xattr(host_path, out) : owner_get_db(dev, ino, out);
}

int dn_policy_owner_set(const char *host_path, dev_t dev, ino_t ino, const DnOwnerRecord *record)
{
	int status = ensure_ready();
	if (status < 0)
		return status;
	return backend_is_xattr ? owner_set_xattr(host_path, record) : owner_set_db(dev, ino, record);
}

int dn_policy_owner_forget(const char *host_path, dev_t dev, ino_t ino)
{
	int status = ensure_ready();
	if (status < 0)
		return status;
	if (backend_is_xattr) {
		/* The record lives on the file; it is already gone along
		 * with the last link. Nothing to do (see dn-policy.h). */
		(void) host_path;
		return 0;
	}
	return owner_forget_db(dev, ino);
}

void dn_policy_fake_stat(const char *host_path, struct stat *st)
{
	DnOwnerRecord record;

	/* Hardlink fixup first: it may replace *st wholesale with the
	 * hidden file's own stat(), so the owner lookup below sees that
	 * file's real (dev, ino) -- the same for every hardlinked name,
	 * matching real hardlink semantics (one owner per inode, not per
	 * name). Its own failure is not fatal to the rest of fake_stat:
	 * it only means "not a managed name" or "couldn't tell", and
	 * either way *st is then whatever the caller's own real stat/
	 * lstat already gave us. */
	(void) dn_policy_hardlink_fixup_stat(host_path, st);

	if (dn_policy_owner_get(host_path, st->st_dev, st->st_ino, &record) < 0) {
		/* No record: "an ordinary Debian install" (runtime.md). */
		st->st_uid = 0;
		st->st_gid = 0;
		return;
	}

	st->st_uid = record.uid;
	st->st_gid = record.gid;
	st->st_mode = (st->st_mode & ~07000) | (record.mode_bits & 07000);
}

/* ---- Fake security.* xattrs: always the DB, never the real xattr -- */

int dn_policy_is_fake_xattr(const char *name)
{
	return strncmp(name, "security.", 9) == 0;
}

/* Variable-length value, fixed-size slot (256 bytes is generous for
 * anything security.* carries in practice, e.g. a capability set). */
#define DN_XATTR_VALUE_CAP 256
#define DN_XATTR_NAME_CAP 64

typedef struct {
	uint64_t dev;
	uint64_t ino;
	char     name[DN_XATTR_NAME_CAP];
	uint32_t size;
	uint8_t  valid;
	char     value[DN_XATTR_VALUE_CAP];
} XattrRecord;

static int xattr_find(int fd, uint64_t dev, uint64_t ino, const char *name,
		XattrRecord *out, off_t *offset_out)
{
	XattrRecord rec;
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
		if (rec.valid && rec.dev == dev && rec.ino == ino
		    && strncmp(rec.name, name, sizeof(rec.name)) == 0) {
			if (out != NULL)
				*out = rec;
			if (offset_out != NULL)
				*offset_out = offset;
			return 0;
		}
		offset += (off_t) sizeof(rec);
	}
}

int dn_policy_fake_setxattr(const char *host_path, dev_t dev, ino_t ino,
		const char *name, const void *value, size_t size)
{
	XattrRecord rec;
	off_t offset;
	int fd, status;

	(void) host_path;
	status = ensure_ready();
	if (status < 0)
		return status;
	if (size > DN_XATTR_VALUE_CAP || strlen(name) >= DN_XATTR_NAME_CAP)
		return -ENOSPC;

	fd = open(xattrs_db_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -errno;
	if (flock(fd, LOCK_EX) < 0) {
		status = -errno;
		goto out;
	}

	memset(&rec, 0, sizeof(rec));
	rec.dev = dev;
	rec.ino = ino;
	strncpy(rec.name, name, sizeof(rec.name) - 1);
	rec.size = (uint32_t) size;
	rec.valid = 1;
	memcpy(rec.value, value, size);

	status = xattr_find(fd, dev, ino, name, NULL, &offset);
	if (status < 0 && status != -ENOENT)
		goto out;
	if (status == -ENOENT) {
		off_t end = lseek(fd, 0, SEEK_END);
		if (end < 0) {
			status = -errno;
			goto out;
		}
		offset = end;
	}

	status = (pwrite(fd, &rec, sizeof(rec), offset) == (ssize_t) sizeof(rec)) ? 0 : -EIO;

out:
	close(fd);
	return status;
}

int dn_policy_fake_getxattr(const char *host_path, dev_t dev, ino_t ino,
		const char *name, void *value_out, size_t cap)
{
	XattrRecord rec;
	int fd, status;

	(void) host_path;
	status = ensure_ready();
	if (status < 0)
		return status;

	fd = open(xattrs_db_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -errno;
	if (flock(fd, LOCK_EX) < 0) {
		status = -errno;
		goto out;
	}

	status = xattr_find(fd, dev, ino, name, &rec, NULL);
	if (status == 0) {
		if (rec.size > cap) {
			status = -ERANGE;
		} else {
			memcpy(value_out, rec.value, rec.size);
			status = (int) rec.size;
		}
	}

out:
	close(fd);
	return status;
}

int dn_policy_fake_removexattr(const char *host_path, dev_t dev, ino_t ino, const char *name)
{
	XattrRecord rec;
	off_t offset;
	int fd, status;

	(void) host_path;
	status = ensure_ready();
	if (status < 0)
		return status;

	fd = open(xattrs_db_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -errno;
	if (flock(fd, LOCK_EX) < 0) {
		status = -errno;
		goto out;
	}

	status = xattr_find(fd, dev, ino, name, &rec, &offset);
	if (status == 0) {
		rec.valid = 0;
		status = (pwrite(fd, &rec, sizeof(rec), offset) == (ssize_t) sizeof(rec)) ? 0 : -EIO;
	}

out:
	close(fd);
	return status;
}
