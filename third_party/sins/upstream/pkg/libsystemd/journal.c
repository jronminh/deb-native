/*
 * SINS userspace journal: append-only file backend for sd_journal_* so
 * writers and readers share one log without systemd-journald.
 *
 * Override path: SINS_JOURNAL_FILE=/path/to/file
 * Default: /var/log/sins-journal/journal.sins, else /tmp/sins-journal/journal.sins
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <limits.h>
#include <sys/inotify.h>

#if !defined(__x86_64__) || !defined(__linux__)
#error "journal.c is built only for Linux x86-64 in this tree"
#endif

#ifndef SD_JOURNAL_NOP
#define SD_JOURNAL_NOP 0
#define SD_JOURNAL_APPEND 1
#endif

typedef union sd_id128 { uint8_t bytes[16]; } sd_id128_t;

#define SINS_REC_MAGIC 0x4a4e4953u /* "SINJ" LE */
#define SINS_REC_VER 1u

struct sins_rec_hdr {
	uint32_t magic;
	uint32_t version;
	uint64_t realtime_usec;
	uint64_t monotonic_usec;
	uint32_t n_fields;
} __attribute__((packed));

typedef struct {
	char *kv;
	size_t len;
} jfield;

typedef struct {
	uint64_t realtime_usec;
	uint64_t monotonic_usec;
	jfield *fields;
	size_t n_fields;
} jentry;

struct sd_journal {
	char *path;
	jentry *entries;
	size_t n_entries;
	ssize_t cursor;
	int dir;
	size_t data_enum_i;
	uint64_t loaded_size;
	time_t loaded_mtime;

	char **matches;
	size_t n_matches;

	int inotify_fd;
};

typedef struct sd_journal sd_journal;

static pthread_mutex_t g_write_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_realtime_usec(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static uint64_t now_monotonic_usec(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static int ensure_parent_dir(const char *filepath) {
	char *p = strdup(filepath);
	if (!p)
		return -ENOMEM;
	char *slash = strrchr(p, '/');
	if (slash && slash != p) {
		*slash = 0;
		for (char *c = p + 1; *c; c++) {
			if (*c != '/')
				continue;
			*c = 0;
			mkdir(p, 0755);
			*c = '/';
		}
		mkdir(p, 0755);
	}
	free(p);
	return 0;
}

static int default_journal_path(char *out, size_t outsz) {
	const char *e = getenv("SINS_JOURNAL_FILE");
	if (e && e[0]) {
		snprintf(out, outsz, "%s", e);
		return 0;
	}
	const char *candidates[] = {
		"/var/log/sins-journal/journal.sins",
		"/tmp/sins-journal/journal.sins",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		ensure_parent_dir(candidates[i]);
		int fd = open(candidates[i], O_CREAT | O_WRONLY, 0644);
		if (fd >= 0) {
			close(fd);
			snprintf(out, outsz, "%s", candidates[i]);
			return 0;
		}
	}
	snprintf(out, outsz, "/tmp/sins-journal/journal.sins");
	ensure_parent_dir(out);
	return 0;
}

static void free_entry(jentry *e) {
	if (!e)
		return;
	for (size_t i = 0; i < e->n_fields; i++)
		free(e->fields[i].kv);
	free(e->fields);
	memset(e, 0, sizeof(*e));
}

static void journal_free_entries(struct sd_journal *j) {
	if (!j->entries)
		return;
	for (size_t i = 0; i < j->n_entries; i++)
		free_entry(&j->entries[i]);
	free(j->entries);
	j->entries = NULL;
	j->n_entries = 0;
}

static int entry_matches(struct sd_journal *j, jentry *e) {
	if (!j->n_matches)
		return 1;
	for (size_t m = 0; m < j->n_matches; m++) {
		const char *pat = j->matches[m];
		int found = 0;
		for (size_t i = 0; i < e->n_fields; i++) {
			if (strstr(e->fields[i].kv, pat) != NULL) {
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
	}
	return 1;
}

static int parse_file_into(struct sd_journal *j) {
	journal_free_entries(j);
	int fd = open(j->path, O_RDONLY);
	if (fd < 0) {
		j->entries = NULL;
		j->n_entries = 0;
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return -errno;
	}
	j->loaded_size = (uint64_t)st.st_size;
	j->loaded_mtime = st.st_mtime;

	if (st.st_size == 0) {
		close(fd);
		return 0;
	}
	uint8_t *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -errno;

	size_t cap = 16;
	j->entries = calloc(cap, sizeof(jentry));
	if (!j->entries) {
		munmap(p, (size_t)st.st_size);
		return -ENOMEM;
	}
	j->n_entries = 0;

	size_t off = 0;
	while (off + sizeof(struct sins_rec_hdr) <= (size_t)st.st_size) {
		struct sins_rec_hdr hdr;
		memcpy(&hdr, p + off, sizeof(hdr));
		if (hdr.magic != SINS_REC_MAGIC || hdr.version != SINS_REC_VER)
			break;
		off += sizeof(hdr);

		jentry ent = {0};
		ent.realtime_usec = hdr.realtime_usec;
		ent.monotonic_usec = hdr.monotonic_usec;
		ent.n_fields = hdr.n_fields;
		ent.fields = calloc(ent.n_fields, sizeof(jfield));
		if (!ent.fields)
			break;

		int bad = 0;
		for (uint32_t fi = 0; fi < hdr.n_fields; fi++) {
			if (off + 4 > (size_t)st.st_size) {
				bad = 1;
				break;
			}
			uint16_t kl, vl;
			memcpy(&kl, p + off, 2);
			memcpy(&vl, p + off + 2, 2);
			off += 4;
			if (off + kl + vl > (size_t)st.st_size) {
				bad = 1;
				break;
			}
			char *kv = malloc((size_t)kl + 1 + (size_t)vl + 1);
			if (!kv) {
				bad = 1;
				break;
			}
			memcpy(kv, p + off, kl);
			kv[kl] = '=';
			memcpy(kv + kl + 1, p + off + kl, vl);
			kv[kl + 1 + vl] = 0;
			off += kl + vl;
			ent.fields[fi].kv = kv;
			ent.fields[fi].len = (size_t)kl + 1 + (size_t)vl;
		}
		if (bad) {
			free_entry(&ent);
			break;
		}
		if (entry_matches(j, &ent)) {
			if (j->n_entries >= cap) {
				cap *= 2;
				jentry *ne = realloc(j->entries, cap * sizeof(jentry));
				if (!ne) {
					free_entry(&ent);
					break;
				}
				j->entries = ne;
			}
			j->entries[j->n_entries++] = ent;
		} else {
			free_entry(&ent);
		}
	}

	munmap(p, (size_t)st.st_size);
	return 0;
}

static int append_record(const char *path, uint64_t rt, uint64_t mon,
    const char *const *kv_pairs, size_t n_pairs) {
	pthread_mutex_lock(&g_write_mu);
	int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (fd < 0) {
		int e = -errno;
		pthread_mutex_unlock(&g_write_mu);
		return e;
	}
	if (flock(fd, LOCK_EX) != 0) {
		int e = -errno;
		close(fd);
		pthread_mutex_unlock(&g_write_mu);
		return e;
	}

	struct sins_rec_hdr hdr = {
	    .magic = SINS_REC_MAGIC,
	    .version = SINS_REC_VER,
	    .realtime_usec = rt,
	    .monotonic_usec = mon,
	    .n_fields = (uint32_t)n_pairs,
	};
	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
		int e = -errno;
		flock(fd, LOCK_UN);
		close(fd);
		pthread_mutex_unlock(&g_write_mu);
		return e;
	}

	for (size_t i = 0; i < n_pairs; i++) {
		const char *eq = strchr(kv_pairs[i], '=');
		if (!eq) {
			flock(fd, LOCK_UN);
			close(fd);
			pthread_mutex_unlock(&g_write_mu);
			return -EINVAL;
		}
		size_t kl = (size_t)(eq - kv_pairs[i]);
		size_t vl = strlen(eq + 1);
		uint16_t k16 = (uint16_t)kl;
		uint16_t v16 = (uint16_t)vl;
		if ((size_t)k16 != kl || (size_t)v16 != vl) {
			flock(fd, LOCK_UN);
			close(fd);
			pthread_mutex_unlock(&g_write_mu);
			return -E2BIG;
		}
		if (write(fd, &k16, 2) != 2 || write(fd, &v16, 2) != 2 ||
		    write(fd, kv_pairs[i], kl) != (ssize_t)kl ||
		    write(fd, eq + 1, vl) != (ssize_t)vl) {
			int e = -errno;
			flock(fd, LOCK_UN);
			close(fd);
			pthread_mutex_unlock(&g_write_mu);
			return e;
		}
	}

	fsync(fd);
	flock(fd, LOCK_UN);
	close(fd);
	pthread_mutex_unlock(&g_write_mu);
	return 0;
}

#define MAX_SEND_FIELDS 48

struct send_bld {
	char *items[MAX_SEND_FIELDS];
	size_t n;
};

static void send_bld_clear(struct send_bld *b) {
	for (size_t i = 0; i < b->n; i++)
		free(b->items[i]);
	b->n = 0;
}

static int send_bld_add(struct send_bld *b, char *owned) {
	if (b->n >= MAX_SEND_FIELDS) {
		free(owned);
		return -E2BIG;
	}
	b->items[b->n++] = owned;
	return 0;
}

static int send_bld_add_fmt(struct send_bld *b, const char *fmt, ...) {
	char *buf = malloc(4096);
	if (!buf)
		return -ENOMEM;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, 4096, fmt, ap);
	va_end(ap);
	return send_bld_add(b, buf);
}

static int one_percent_s_only(const char *fmt) {
	const char *p = fmt;
	int n = 0;
	while (*p) {
		if (p[0] == '%') {
			if (p[1] == '%') {
				p += 2;
				continue;
			}
			if (p[1] == 's')
				n++;
			else
				return -1;
			p += 2;
			continue;
		}
		p++;
	}
	return n == 1 ? 0 : -1;
}

static int one_percent_int_only(const char *fmt) {
	const char *p = fmt;
	int n = 0;
	while (*p) {
		if (p[0] == '%') {
			if (p[1] == '%') {
				p += 2;
				continue;
			}
			if (p[1] == 'i' || p[1] == 'd')
				n++;
			else
				return -1;
			p += 2;
			continue;
		}
		p++;
	}
	return n == 1 ? 0 : -1;
}

static int journal_sendv_iov(const struct iovec *iov, int n) {
	char path[512];
	default_journal_path(path, sizeof(path));
	uint64_t rt = now_realtime_usec();
	uint64_t mon = now_monotonic_usec();
	const char *tmp[MAX_SEND_FIELDS];
	size_t nt = 0;
	for (int i = 0; i < n && nt < MAX_SEND_FIELDS; i++) {
		if (!iov[i].iov_base || iov[i].iov_len == 0)
			continue;
		char *copy = malloc(iov[i].iov_len + 1);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, iov[i].iov_base, iov[i].iov_len);
		copy[iov[i].iov_len] = 0;
		tmp[nt++] = copy;
	}
	int r = append_record(path, rt, mon, tmp, nt);
	for (size_t i = 0; i < nt; i++)
		free((void *)tmp[i]);
	return r;
}

int sd_journal_send(const char *fmt, ...) {
	if (!fmt)
		return -EINVAL;
	char path[512];
	default_journal_path(path, sizeof(path));
	struct send_bld b = {0};
	va_list ap;
	va_start(ap, fmt);
	const char *f = fmt;
	int err = 0;
	while (f) {
		if (!strchr(f, '%')) {
			if (send_bld_add(&b, strdup(f)) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		if (one_percent_s_only(f) == 0) {
			const char *s = va_arg(ap, const char *);
			if (send_bld_add_fmt(&b, f, s) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		if (one_percent_int_only(f) == 0) {
			int x = va_arg(ap, int);
			if (send_bld_add_fmt(&b, f, x) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		err = -ENOTSUP;
		break;
	}
	va_end(ap);
	if (err) {
		send_bld_clear(&b);
		return err;
	}
	const char *const *ptrs = (const char *const *)b.items;
	int r = append_record(path, now_realtime_usec(), now_monotonic_usec(), ptrs, b.n);
	send_bld_clear(&b);
	return r;
}

int sd_journal_send_with_location(const char *file, int line, const char *func,
    const char *fmt, ...) {
	(void)file;
	(void)line;
	(void)func;
	char path[512];
	default_journal_path(path, sizeof(path));
	struct send_bld b = {0};
	va_list ap;
	va_start(ap, fmt);
	const char *f = fmt;
	int err = 0;
	while (f) {
		if (!strchr(f, '%')) {
			if (send_bld_add(&b, strdup(f)) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		if (one_percent_s_only(f) == 0) {
			const char *s = va_arg(ap, const char *);
			if (send_bld_add_fmt(&b, f, s) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		if (one_percent_int_only(f) == 0) {
			int x = va_arg(ap, int);
			if (send_bld_add_fmt(&b, f, x) < 0) {
				err = -E2BIG;
				break;
			}
			f = va_arg(ap, const char *);
			continue;
		}
		err = -ENOTSUP;
		break;
	}
	va_end(ap);
	if (err) {
		send_bld_clear(&b);
		return err;
	}
	const char *const *ptrs = (const char *const *)b.items;
	int r = append_record(path, now_realtime_usec(), now_monotonic_usec(), ptrs, b.n);
	send_bld_clear(&b);
	return r;
}

int sd_journal_sendv(const struct iovec *iov, int n) {
	return journal_sendv_iov(iov, n);
}

int sd_journal_sendv_with_location(const struct iovec *iov, int n,
    const char *file, int line, const char *func) {
	(void)file;
	(void)line;
	(void)func;
	return journal_sendv_iov(iov, n);
}

int sd_journal_print(int priority, const char *format, ...) {
	char msg[8192];
	va_list ap;
	va_start(ap, format);
	vsnprintf(msg, sizeof msg, format, ap);
	va_end(ap);
	char path[512];
	default_journal_path(path, sizeof(path));
	char *mline = malloc(strlen(msg) + 16);
	char pline[32];
	if (!mline)
		return -ENOMEM;
	snprintf(mline, strlen(msg) + 16, "MESSAGE=%s", msg);
	snprintf(pline, sizeof pline, "PRIORITY=%d", priority & 7);
	const char *pairs[2] = { mline, pline };
	int r = append_record(path, now_realtime_usec(), now_monotonic_usec(), pairs, 2);
	free(mline);
	return r;
}

int sd_journal_print_with_location(int priority, const char *file, int line,
    const char *func, const char *format, ...) {
	(void)file;
	(void)line;
	(void)func;
	va_list ap;
	va_start(ap, format);
	char msg[8192];
	vsnprintf(msg, sizeof msg, format, ap);
	va_end(ap);
	char path[512];
	default_journal_path(path, sizeof(path));
	char *mline = malloc(strlen(msg) + 16);
	char pline[32];
	if (!mline)
		return -ENOMEM;
	snprintf(mline, strlen(msg) + 16, "MESSAGE=%s", msg);
	snprintf(pline, sizeof pline, "PRIORITY=%d", priority & 7);
	const char *pairs[2] = { mline, pline };
	int r = append_record(path, now_realtime_usec(), now_monotonic_usec(), pairs, 2);
	free(mline);
	return r;
}

int sd_journal_printv(int priority, const char *format, va_list ap) {
	char msg[8192];
	vsnprintf(msg, sizeof msg, format, ap);
	char path[512];
	default_journal_path(path, sizeof(path));
	char *mline = malloc(strlen(msg) + 16);
	char pline[32];
	if (!mline)
		return -ENOMEM;
	snprintf(mline, strlen(msg) + 16, "MESSAGE=%s", msg);
	snprintf(pline, sizeof pline, "PRIORITY=%d", priority & 7);
	const char *pairs[2] = { mline, pline };
	int r = append_record(path, now_realtime_usec(), now_monotonic_usec(), pairs, 2);
	free(mline);
	return r;
}

int sd_journal_printv_with_location(int priority, const char *file, int line,
    const char *func, const char *format, va_list ap) {
	(void)file;
	(void)line;
	(void)func;
	return sd_journal_printv(priority, format, ap);
}

int sd_journal_perror(const char *message) {
	char path[512];
	default_journal_path(path, sizeof(path));
	char buf[1024];
	snprintf(buf, sizeof buf, "MESSAGE=%s: %s", message ? message : "error", strerror(errno));
	const char *pairs[2] = { buf, "PRIORITY=3" };
	return append_record(path, now_realtime_usec(), now_monotonic_usec(), pairs, 2);
}

int sd_journal_perror_with_location(const char *file, int line, const char *func,
    const char *message) {
	(void)file;
	(void)line;
	(void)func;
	return sd_journal_perror(message);
}

static struct sd_journal *journal_new_empty(const char *path) {
	struct sd_journal *j = calloc(1, sizeof(*j));
	if (!j)
		return NULL;
	j->path = strdup(path);
	if (!j->path) {
		free(j);
		return NULL;
	}
	j->cursor = -1;
	j->dir = 1;
	j->inotify_fd = -1;
	return j;
}

static void journal_setup_notify(sd_journal *j) {
	j->inotify_fd = -1;
	char *dir = strdup(j->path);
	if (!dir)
		return;
	char *slash = strrchr(dir, '/');
	if (!slash) {
		free(dir);
		return;
	}
	*slash = 0;
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (fd < 0) {
		free(dir);
		return;
	}
	uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF |
	    IN_MOVE_SELF;
	if (inotify_add_watch(fd, dir, mask) < 0) {
		close(fd);
		free(dir);
		return;
	}
	j->inotify_fd = fd;
	free(dir);
}

static void journal_shutdown(sd_journal *j) {
	if (j->inotify_fd >= 0) {
		close(j->inotify_fd);
		j->inotify_fd = -1;
	}
}

int sd_journal_open(sd_journal **ret, int flags) {
	(void)flags;
	if (!ret)
		return -EINVAL;
	char path[512];
	default_journal_path(path, sizeof(path));
	struct sd_journal *j = journal_new_empty(path);
	if (!j)
		return -ENOMEM;
	int r = parse_file_into(j);
	if (r < 0) {
		free(j->path);
		free(j);
		return r;
	}
	journal_setup_notify(j);
	*ret = j;
	return 0;
}

int sd_journal_open_directory(sd_journal **ret, const char *path, int flags) {
	(void)flags;
	if (!ret || !path)
		return -EINVAL;
	char buf[1024];
	snprintf(buf, sizeof buf, "%s/journal.sins", path);
	ensure_parent_dir(buf);
	struct sd_journal *j = journal_new_empty(buf);
	if (!j)
		return -ENOMEM;
	int r = parse_file_into(j);
	if (r < 0) {
		free(j->path);
		free(j);
		return r;
	}
	journal_setup_notify(j);
	*ret = j;
	return 0;
}

int sd_journal_open_directory_fd(sd_journal **ret, int fd, int flags) {
	(void)flags;
	if (!ret || fd < 0)
		return -EINVAL;
	char procpath[64];
	snprintf(procpath, sizeof procpath, "/proc/self/fd/%d", fd);
	char pathbuf[4096];
	ssize_t pl = readlink(procpath, pathbuf, sizeof(pathbuf) - 1);
	if (pl < 0)
		return -errno;
	pathbuf[pl] = 0;
	char jpath[4352];
	snprintf(jpath, sizeof jpath, "%s/journal.sins", pathbuf);
	struct sd_journal *j = journal_new_empty(jpath);
	if (!j)
		return -ENOMEM;
	int r = parse_file_into(j);
	if (r < 0) {
		free(j->path);
		free(j);
		return r;
	}
	journal_setup_notify(j);
	*ret = j;
	return 0;
}

int sd_journal_open_files(sd_journal **ret, const char **paths, int flags) {
	(void)flags;
	if (!ret || !paths || !paths[0])
		return -EINVAL;
	struct sd_journal *j = journal_new_empty(paths[0]);
	if (!j)
		return -ENOMEM;
	int r = parse_file_into(j);
	if (r < 0) {
		free(j->path);
		free(j);
		return r;
	}
	journal_setup_notify(j);
	*ret = j;
	return 0;
}

int sd_journal_open_files_fd(sd_journal **ret, int *fds, unsigned n_fds, int flags) {
	(void)flags;
	if (!ret || !fds || n_fds == 0)
		return -EINVAL;
	char procpath[64];
	snprintf(procpath, sizeof procpath, "/proc/self/fd/%d", fds[0]);
	char pathbuf[4096];
	ssize_t pl = readlink(procpath, pathbuf, sizeof(pathbuf) - 1);
	if (pl < 0)
		return -errno;
	pathbuf[pl] = 0;
	struct sd_journal *j = journal_new_empty(pathbuf);
	if (!j)
		return -ENOMEM;
	int r = parse_file_into(j);
	if (r < 0) {
		free(j->path);
		free(j);
		return r;
	}
	journal_setup_notify(j);
	*ret = j;
	return 0;
}

int sd_journal_open_namespace(sd_journal **ret, const char *namespace, int flags) {
	(void)namespace;
	return sd_journal_open(ret, flags);
}

int sd_journal_open_container(sd_journal **ret, int machine_fd, int flags) {
	(void)machine_fd;
	return sd_journal_open(ret, flags);
}

void sd_journal_flush_matches(sd_journal *j) {
	if (!j)
		return;
	for (size_t i = 0; i < j->n_matches; i++)
		free(j->matches[i]);
	free(j->matches);
	j->matches = NULL;
	j->n_matches = 0;
}

int sd_journal_add_match(sd_journal *j, const void *data, size_t size) {
	if (!j || !data)
		return -EINVAL;
	char *s = malloc(size + 1);
	if (!s)
		return -ENOMEM;
	memcpy(s, data, size);
	s[size] = 0;
	char **nm = realloc(j->matches, (j->n_matches + 1) * sizeof(char *));
	if (!nm) {
		free(s);
		return -ENOMEM;
	}
	j->matches = nm;
	j->matches[j->n_matches++] = s;
	return 0;
}

int sd_journal_add_disjunction(sd_journal *j) {
	(void)j;
	return 0;
}

int sd_journal_add_conjunction(sd_journal *j) {
	(void)j;
	return 0;
}

void sd_journal_close(sd_journal *j) {
	if (!j)
		return;
	sd_journal_flush_matches(j);
	journal_shutdown(j);
	journal_free_entries(j);
	free(j->path);
	free(j);
}

int sd_journal_seek_head(sd_journal *j) {
	if (!j)
		return -EINVAL;
	j->dir = 1;
	j->cursor = -1;
	return 0;
}

int sd_journal_seek_tail(sd_journal *j) {
	if (!j)
		return -EINVAL;
	j->dir = -1;
	j->cursor = (ssize_t)j->n_entries;
	return 0;
}

int sd_journal_seek_monotonic_usec(sd_journal *j, uint64_t usec, sd_id128_t boot_id) {
	(void)boot_id;
	if (!j)
		return -EINVAL;
	for (size_t i = 0; i < j->n_entries; i++) {
		if (j->entries[i].monotonic_usec >= usec) {
			j->dir = 1;
			j->cursor = (ssize_t)i - 1;
			return 0;
		}
	}
	j->cursor = (ssize_t)j->n_entries - 1;
	return 0;
}

int sd_journal_seek_realtime_usec(sd_journal *j, uint64_t usec) {
	if (!j)
		return -EINVAL;
	for (size_t i = 0; i < j->n_entries; i++) {
		if (j->entries[i].realtime_usec >= usec) {
			j->dir = 1;
			j->cursor = (ssize_t)i - 1;
			return 0;
		}
	}
	j->cursor = (ssize_t)j->n_entries - 1;
	return 0;
}

int sd_journal_seek_cursor(sd_journal *j, const char *cursor) {
	if (!j || !cursor)
		return -EINVAL;
	unsigned long idx = 0;
	if (sscanf(cursor, "sins:%lu", &idx) != 1)
		return -EINVAL;
	if (idx >= j->n_entries)
		return -EINVAL;
	j->dir = 1;
	j->cursor = (ssize_t)idx - 1;
	return 0;
}

int sd_journal_next(sd_journal *j) {
	if (!j)
		return -EINVAL;
	if (j->dir == 1) {
		if (j->cursor < (ssize_t)j->n_entries - 1) {
			j->cursor++;
			return 1;
		}
		return 0;
	}
	if (j->n_entries == 0)
		return 0;
	if (j->cursor == (ssize_t)j->n_entries) {
		j->cursor = (ssize_t)j->n_entries - 1;
		return 1;
	}
	if (j->cursor > 0) {
		j->cursor--;
		return 1;
	}
	return 0;
}

int sd_journal_next_skip(sd_journal *j, uint64_t skip) {
	for (uint64_t i = 0; i < skip; i++) {
		if (sd_journal_next(j) <= 0)
			return 0;
	}
	return 1;
}

int sd_journal_previous(sd_journal *j) {
	if (!j)
		return -EINVAL;
	if (j->dir == 1) {
		if (j->cursor >= 0) {
			j->cursor--;
			return 1;
		}
		return 0;
	}
	if (j->n_entries == 0)
		return 0;
	if (j->cursor < (ssize_t)j->n_entries - 1) {
		j->cursor++;
		return 1;
	}
	return 0;
}

int sd_journal_previous_skip(sd_journal *j, uint64_t skip) {
	for (uint64_t i = 0; i < skip; i++) {
		if (sd_journal_previous(j) <= 0)
			return 0;
	}
	return 1;
}

static jentry *current_entry(struct sd_journal *j) {
	if (!j || j->cursor < 0 || (size_t)j->cursor >= j->n_entries)
		return NULL;
	return &j->entries[j->cursor];
}

int sd_journal_get_data(sd_journal *j, const char *field, const void **data, size_t *length) {
	if (!j || !field || !data || !length)
		return -EINVAL;
	jentry *e = current_entry(j);
	if (!e)
		return -ENOENT;
	size_t fl = strlen(field);
	for (size_t i = 0; i < e->n_fields; i++) {
		if (e->fields[i].len > fl && strncmp(e->fields[i].kv, field, fl) == 0 &&
		    e->fields[i].kv[fl] == '=') {
			*data = e->fields[i].kv;
			*length = e->fields[i].len;
			return 0;
		}
	}
	return -ENOENT;
}

void sd_journal_restart_data(sd_journal *j) {
	if (j)
		j->data_enum_i = 0;
}

int sd_journal_enumerate_data(sd_journal *j, const void **data, size_t *length) {
	if (!j || !data || !length)
		return -EINVAL;
	jentry *e = current_entry(j);
	if (!e)
		return -ENOENT;
	if (j->data_enum_i >= e->n_fields)
		return 0;
	*data = e->fields[j->data_enum_i].kv;
	*length = e->fields[j->data_enum_i].len;
	j->data_enum_i++;
	return 1;
}

int sd_journal_enumerate_available_data(sd_journal *j, const void **data, size_t *length) {
	return sd_journal_enumerate_data(j, data, length);
}

int sd_journal_get_realtime_usec(sd_journal *j, uint64_t *usec) {
	if (!usec)
		return -EINVAL;
	jentry *e = current_entry(j);
	if (!e)
		return -ENOENT;
	*usec = e->realtime_usec;
	return 0;
}

int sd_journal_get_monotonic_usec(sd_journal *j, uint64_t *usec, sd_id128_t *boot_id) {
	if (boot_id)
		memset(boot_id, 0, sizeof(sd_id128_t));
	if (!usec)
		return -EINVAL;
	jentry *e = current_entry(j);
	if (!e)
		return -ENOENT;
	*usec = e->monotonic_usec;
	return 0;
}

int sd_journal_get_cursor(sd_journal *j, char **cursor) {
	if (!j || !cursor)
		return -EINVAL;
	if (j->cursor < 0 || (size_t)j->cursor >= j->n_entries)
		return -EINVAL;
	char buf[64];
	snprintf(buf, sizeof buf, "sins:%zu", (size_t)j->cursor);
	*cursor = strdup(buf);
	return *cursor ? 0 : -ENOMEM;
}

int sd_journal_test_cursor(sd_journal *j, const char *cursor) {
	if (!j || !cursor)
		return -EINVAL;
	unsigned long idx = 0;
	if (sscanf(cursor, "sins:%lu", &idx) != 1)
		return -EINVAL;
	if (idx == (unsigned long)j->cursor)
		return 1;
	return 0;
}

int sd_journal_process(sd_journal *j) {
	if (!j)
		return -EINVAL;
	struct stat st;
	if (stat(j->path, &st) < 0)
		return SD_JOURNAL_NOP;
	int changed = (uint64_t)st.st_size != j->loaded_size || st.st_mtime != j->loaded_mtime;
	if (!changed)
		return SD_JOURNAL_NOP;
	ssize_t saved = j->cursor;
	int d = j->dir;
	parse_file_into(j);
	j->dir = d;
	if (saved >= 0 && (size_t)saved < j->n_entries)
		j->cursor = saved;
	else
		j->cursor = -1;
	return SD_JOURNAL_APPEND;
}

static void journal_drain_inotify(sd_journal *j) {
	if (!j || j->inotify_fd < 0)
		return;
	char buf[4096];
	while (read(j->inotify_fd, buf, sizeof buf) > 0)
		;
}

int sd_journal_get_fd(sd_journal *j) {
	if (!j)
		return -EINVAL;
	if (j->inotify_fd < 0)
		return -ENOTSUP;
	return j->inotify_fd;
}

int sd_journal_get_events(sd_journal *j) {
	(void)j;
	return POLLIN;
}

int sd_journal_get_timeout(sd_journal *j, uint64_t *timeout_usec) {
	if (!j)
		return -EINVAL;
	if (timeout_usec) {
		if (j->inotify_fd >= 0)
			*timeout_usec = UINT64_MAX;
		else
			*timeout_usec = (uint64_t)-1;
	}
	return 1;
}

int sd_journal_wait(sd_journal *j, uint64_t timeout_usec) {
	if (!j)
		return -EINVAL;
	int poll_ms = -1;
	if (timeout_usec != UINT64_MAX) {
		uint64_t ms64 = timeout_usec / 1000ULL;
		poll_ms = ms64 > (uint64_t)INT_MAX ? INT_MAX : (int)ms64;
	}
	if (j->inotify_fd >= 0) {
		struct pollfd p = { .fd = j->inotify_fd, .events = POLLIN };
		poll(&p, 1, poll_ms);
		if (p.revents & POLLIN)
			journal_drain_inotify(j);
	} else {
		useconds_t us = 100000;
		if (poll_ms >= 0) {
			us = (useconds_t)poll_ms * 1000;
			if (us > 1000000)
				us = 1000000;
			if (us == 0)
				us = 1000;
		}
		usleep(us);
	}
	return sd_journal_process(j);
}

int sd_journal_reliable_fd(sd_journal *j) {
	return j && j->inotify_fd >= 0;
}

int sd_journal_get_usage(sd_journal *j, uint64_t *bytes) {
	if (!j || !bytes)
		return -EINVAL;
	struct stat st;
	if (stat(j->path, &st) < 0)
		return -errno;
	*bytes = (uint64_t)st.st_size;
	return 0;
}

int sd_journal_get_seqnum(sd_journal *j, uint64_t *ret) {
	if (!ret)
		return -EINVAL;
	if (j->cursor < 0 || (size_t)j->cursor >= j->n_entries)
		return -ENOENT;
	*ret = (uint64_t)j->cursor;
	return 0;
}

int sd_journal_get_cutoff_realtime_usec(sd_journal *j, uint64_t *from, uint64_t *to) {
	if (!j || !from || !to)
		return -EINVAL;
	if (j->n_entries == 0)
		return -ENOENT;
	*from = j->entries[0].realtime_usec;
	*to = j->entries[j->n_entries - 1].realtime_usec;
	return 0;
}

int sd_journal_get_cutoff_monotonic_usec(sd_journal *j, uint64_t *from, uint64_t *to) {
	if (!j || !from || !to)
		return -EINVAL;
	if (j->n_entries == 0)
		return -ENOENT;
	*from = j->entries[0].monotonic_usec;
	*to = j->entries[j->n_entries - 1].monotonic_usec;
	return 0;
}

int sd_journal_step_one(sd_journal *j, int more, int err) {
	(void)more;
	(void)err;
	return sd_journal_next(j);
}

void sd_journal_restart_unique(sd_journal *j) {
	(void)j;
}

int sd_journal_enumerate_unique(sd_journal *j, const void **data, size_t *length) {
	(void)j;
	(void)data;
	(void)length;
	return 0;
}

void sd_journal_restart_fields(sd_journal *j) {
	(void)j;
}

int sd_journal_enumerate_fields(sd_journal *j, const char **field) {
	(void)j;
	(void)field;
	return 0;
}

int sd_journal_query_unique(sd_journal *j, const char *field) {
	(void)j;
	(void)field;
	return 0;
}

int sd_journal_enumerate_available_unique(sd_journal *j, const void **data, size_t *length) {
	return sd_journal_enumerate_unique(j, data, length);
}

int sd_journal_get_catalog(sd_journal *j) {
	(void)j;
	return -ENOENT;
}

int sd_journal_get_catalog_for_message_id(sd_journal *j, sd_id128_t id) {
	(void)j;
	(void)id;
	return -ENOENT;
}

int sd_journal_get_data_threshold(sd_journal *j, size_t *sz) {
	(void)j;
	if (sz)
		*sz = 0;
	return 0;
}

int sd_journal_set_data_threshold(sd_journal *j, size_t sz) {
	(void)j;
	(void)sz;
	return 0;
}

int sd_journal_has_persistent_files(sd_journal *j) {
	(void)j;
	return 1;
}

int sd_journal_has_runtime_files(sd_journal *j) {
	(void)j;
	return 1;
}

int sd_journal_stream_fd(sd_journal *j, int priority, int level_prefix) {
	(void)j;
	(void)priority;
	(void)level_prefix;
	return -ENOTSUP;
}

int sd_journal_stream_fd_with_namespace(sd_journal *j, const char *namespace,
    int priority, int level_prefix) {
	(void)j;
	(void)namespace;
	(void)priority;
	(void)level_prefix;
	return -ENOTSUP;
}
