/* policy.c -- the table and the law, in their smallest form. */
#include "policy.h"

static const struct row ROWS[] = {
#include "policy-rows.inc"        /* generated: the caught syscalls and what is done with each */
};
#define NROWS (sizeof ROWS / sizeof ROWS[0])

static const struct row *by_nr[512];
static int indexed;

static const char *root;
static size_t root_len;
static char loader[512], libpath[1024], preload[512];

/* ---- the link cache (policy.h).  An entry is valid while its generation is the
 * shared one; moving the generation drops every entry at once. ------------------ */
#define LC_SLOTS 4096
#define LC_KEY   192
#define LC_TGT   128
struct lc {
	unsigned gen;                 /* 0: empty */
	unsigned hash;
	int r;                        /* what readlink returned: <= 0 not a link, else the target's length */
	char key[LC_KEY];
	char tgt[LC_TGT];
};
static struct lc lcache[LC_SLOTS];
static volatile unsigned *links_gen;  /* in the shared file; NULL: no cache */

static void links_init(void)
{
	char p[600];
	int fd;
	i64 m;

	links_gen = NULL;
	if (cat2(p, sizeof p, root, "/var/lib/deb-native/dn-tiny.gen") <= 0)
		return;
	fd = (int) sc(56, -100, (i64) p, 02 | 0100, 0600, 0, 0);          /* openat O_RDWR|O_CREAT */
	if (fd < 0)
		return;
	if (sc(46, fd, 4096, 0, 0, 0, 0) == 0) {                            /* ftruncate */
		m = sc(222, 0, 4096, 3, 1, fd, 0);                          /* mmap PROT_READ|WRITE, MAP_SHARED */
		if (m > 0 || m < -4095)
			links_gen = (volatile unsigned *) m;
	}
	sys_close(fd);
	if (links_gen && *links_gen == 0)
		policy_links_changed();                                     /* generation 0 marks an empty slot */
}

void policy_links_changed(void)
{
	unsigned tmp, fail;

	if (!links_gen)
		return;
	__asm__ volatile("1: ldaxr %w0, [%2]\n"
			 "   add   %w0, %w0, #1\n"
			 "   stlxr %w1, %w0, [%2]\n"
			 "   cbnz  %w1, 1b\n"
			 : "=&r"(tmp), "=&r"(fail) : "r"(links_gen) : "memory");
}

unsigned policy_links_gen(void) { return links_gen ? *links_gen : 0; }

/* readlink(host path), from the cache when it holds the answer */
static ssize_t lc_readlink(const char *p, char *buf, size_t n)
{
	unsigned h = 2166136261u, g;
	size_t len = 0;
	struct lc *e;
	ssize_t r;

	if (!links_gen)
		return sys_readlink(p, buf, n);
	while (p[len]) {
		h = (h ^ (unsigned char) p[len]) * 16777619u;
		len++;
	}
	g = *links_gen;
	e = &lcache[h % LC_SLOTS];
	if (e->gen == g && e->hash == h && strcmp(e->key, p) == 0) {
		if (e->r <= 0)
			return e->r;
		if ((size_t) e->r <= n) {
			memcpy(buf, e->tgt, (size_t) e->r);
			return e->r;
		}
	}
	r = sys_readlink(p, buf, n);
	if (len < LC_KEY && r < LC_TGT) {
		e->gen = g;
		e->hash = h;
		e->r = (int) r;
		memcpy(e->key, p, len + 1);
		if (r > 0)
			memcpy(e->tgt, buf, (size_t) r);
	}
	return r;
}

size_t policy_count(void) { return NROWS; }
const struct row *policy_at(size_t i) { return &ROWS[i]; }

const struct row *policy_row(unsigned nr)
{
	size_t i;

	if (!indexed) {
		for (i = 0; i < NROWS; i++)
			if (ROWS[i].nr < 512)
				by_nr[ROWS[i].nr] = &ROWS[i];
		indexed = 1;
	}
	return nr < 512 ? by_nr[nr] : NULL;
}

/* append s to the string in buf (capacity cap) */
static void append(char *buf, size_t cap, const char *s)
{
	size_t n = strlen(buf);

	scpy(buf + n, cap - n, s);
}

void policy_set_root(const char *r)
{
	root = r;
	root_len = strlen(r);
	loader[0] = libpath[0] = '\0';
	append(loader, sizeof loader, r);
	append(loader, sizeof loader, "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1");
	append(libpath, sizeof libpath, r);
	append(libpath, sizeof libpath, "/usr/lib/aarch64-linux-gnu:");
	append(libpath, sizeof libpath, r);
	append(libpath, sizeof libpath, "/usr/lib");
	preload[0] = '\0';
	append(preload, sizeof preload, r);
	append(preload, sizeof preload, "/usr/lib/deb-native/dn-lane.so");
	{
		char st[128];

		if (sys_fstatat(preload, st, 0) != 0)
			preload[0] = '\0';
	}
	links_init();
}

const char *policy_preload(void) { return preload; }
const char *policy_loader(void) { return loader; }
const char *policy_libpath(void) { return libpath; }

int policy_is_glibc_interp(const char *interp)
{
	size_t n = strlen(interp), m = sizeof "ld-linux-aarch64.so.1" - 1;

	return n >= m && strcmp(interp + n - m, "ld-linux-aarch64.so.1") == 0;
}

/* what is already a host path stays */
static int is_host_path(const char *p)
{
	static const char *const keep[] = { "/proc", "/sys", "/dev", "/data", "/system", "/storage",
					    "/sdcard", "/apex", "/vendor", "/mnt", NULL };
	int i;

	if (strncmp(p, root, root_len) == 0)
		return 1;
	for (i = 0; keep[i]; i++) {
		size_t n = strlen(keep[i]);

		if (strncmp(p, keep[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
			return 1;
	}
	return 0;
}

/* ---- symlinks: the kernel would follow an absolute target from the host's /, not
 * the prefix's, so the path is walked here, one component at a time, and a link
 * met on the way is read and its target mapped the same way. -------------------- */
#define MAX_HOPS 40
#define WORK 1024

/* the part of `s` before its last '/', cut in place (never above the root) */
static void pop_component(char *cur)
{
	size_t n = strlen(cur);

	while (n > root_len && cur[n - 1] != '/')
		n--;
	if (n > root_len)
		n--;
	cur[n] = '\0';
}

/* the host path for guest path `in`, links resolved; 0 if `in` is left as it is,
 * else the length (NUL included).  The last component is followed unless `nofollow`. */
static int walk(const char *base, const char *in, char *out, size_t cap, int nofollow)
{
	char cur[512], rest[WORK], tgt[512], *p;
	size_t cl;
	int hops = 0;

	if (scpy(rest, sizeof rest, in) < 0 || scpy(cur, sizeof cur, base) < 0)
		goto plain;
	if (in[0] && in[strlen(in) - 1] == '/')
		nofollow = 0;                                  /* a trailing slash follows the link, as the kernel does */
	p = rest;
	for (;;) {
		char *comp, *after;
		int last;
		ssize_t r;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		comp = p;
		while (*p && *p != '/')
			p++;
		after = p;                                     /* what follows the component */
		while (*after == '/')
			after++;
		last = (*after == '\0');
		if (comp[0] == '.' && p - comp == 1)
			continue;
		if (comp[0] == '.' && comp[1] == '.' && p - comp == 2) {
			pop_component(cur);
			continue;
		}
		cl = strlen(cur);
		if (cl + 1 + (size_t) (p - comp) + 1 > sizeof cur)
			goto plain;
		cur[cl] = '/';
		memcpy(cur + cl + 1, comp, (size_t) (p - comp));
		cur[cl + 1 + (size_t) (p - comp)] = '\0';
		if (last && nofollow)
			continue;                              /* a name to create, remove or read as a link */
		r = lc_readlink(cur, tgt, sizeof tgt - 1);
		if (r <= 0)
			continue;                              /* not a link (or not there): it stays */
		tgt[r] = '\0';
		if (++hops > MAX_HOPS)
			goto plain;                            /* the kernel will say ELOOP */
		cur[cl] = '\0';                                /* back to the directory holding the link */
		{
			char nrest[WORK];
			size_t tl = (size_t) r, al = strlen(after);

			if (tl + 1 + al + 1 > sizeof nrest)
				goto plain;
			memcpy(nrest, tgt, tl);
			nrest[tl] = '/';
			memcpy(nrest + tl + 1, after, al + 1);
			if (tgt[0] == '/') {
				if (strncmp(tgt, root, root_len) == 0 && (tgt[root_len] == '/' || tgt[root_len] == '\0')) {
					/* already a host path inside the prefix: map from the root */
					memmove(nrest, nrest + root_len, strlen(nrest + root_len) + 1);
				} else if (is_host_path(tgt)) {
					return cat2(out, cap, nrest, "") > 0 ? (int) strlen(out) + 1 : 0;
				}
				scpy(cur, sizeof cur, root);
			}
			scpy(rest, sizeof rest, nrest);
			p = rest;
		}
	}
	if (in[strlen(in) - 1] == '/' && strlen(cur) + 2 <= sizeof cur)
		memcpy(cur + strlen(cur), "/", 2);
	return scpy(out, cap, cur) >= 0 ? (int) strlen(out) + 1 : 0;
plain:                                                  /* give the kernel the path, mapped but not resolved */
	if (cat2(out, cap, base, in[0] == '/' ? "" : "/") < 0)
		return 0;
	return cat2(out, cap, out, in) > 0 ? (int) strlen(out) + 1 : 0;
}

int policy_path_nf(const char *in, char *out, size_t cap, int nofollow)
{
	if (in[0] != '/' || is_host_path(in))
		return 0;
	return walk(root, in, out, cap, nofollow);
}

/* a relative path, from a directory the kernel names (a host path): it is walked the
 * same way; 0 if that directory is outside the prefix, where nothing is mapped */
int policy_path_at(const char *base, const char *in, char *out, size_t cap, int nofollow)
{
	size_t n = strlen(base);

	if (in[0] == '/' || strncmp(base, root, root_len) != 0 || (base[root_len] != '/' && base[root_len] != 0))
		return 0;
	if (n > 0 && base[n - 1] == '/')
		return 0;
	return walk(base, in, out, cap, nofollow);
}

/* The register holding the directory fd for the path in register `arg`, or -1 when the
 * path is taken from the cwd (the calls without a directory fd). */
int policy_dirfd_arg(unsigned nr, int arg)
{
	switch (nr) {
	case 33: case 34: case 35: case 36: case 37: case 38: case 276: case 48: case 53: case 54:
	case 56: case 78: case 79: case 88: case 291: case 437: case 439: case 452:
		return arg - 1;
	}
	return -1;
}

int policy_path(const char *in, char *out, size_t cap)
{
	return policy_path_nf(in, out, cap, 0);
}

/* Does the syscall act on the link itself, not what it points to, for the path in
 * register `arg`?  (the last component only; the ones before it are always followed) */
int policy_nofollow(unsigned nr, int arg, const u64 *x)
{
	switch (nr) {
	case 33: case 34: case 35: case 36: case 78:   /* mknodat mkdirat unlinkat symlinkat readlinkat */
	case 38: case 276:                             /* renameat renameat2 */
	case 6: case 9: case 12: case 15:              /* l*xattr */
		return 1;
	case 37:                                       /* linkat: the old name only with AT_SYMLINK_FOLLOW */
		return arg == 3 || !(x[4] & 0x400);
	case 79: return !!(x[3] & 0x100);              /* newfstatat: AT_SYMLINK_NOFOLLOW */
	case 439: return !!(x[3] & 0x100);             /* faccessat2 */
	case 54: return !!(x[4] & 0x100);              /* fchownat */
	case 88: return !!(x[3] & 0x100);              /* utimensat */
	case 291: return !!(x[2] & 0x100);             /* statx */
	case 27: return !!(x[2] & 0x2000000);          /* inotify_add_watch: IN_DONT_FOLLOW */
	case 56:                                       /* openat: O_NOFOLLOW, or O_CREAT with O_EXCL */
		return !!(x[2] & 0x8000) || (x[2] & 0xc0) == 0xc0;
	}
	return 0;
}

int policy_unpath(const char *host, char *out, size_t cap)
{
	if (strncmp(host, root, root_len) != 0 || (host[root_len] != '\0' && host[root_len] != '/'))
		return 0;
	if (host[root_len] == '\0')
		return scpy(out, cap, "/") >= 0 ? 2 : 0;       /* the prefix root is the guest's / */
	{
		int n = scpy(out, cap, host + root_len);

		return n >= 0 ? n + 1 : 0;
	}
}

static unsigned real_uid = (unsigned) -1, real_gid = (unsigned) -1;

void policy_set_ids(unsigned uid, unsigned gid) { real_uid = uid; real_gid = gid; }
unsigned policy_map_uid(unsigned id) { return id == real_uid ? 0 : id; }
unsigned policy_map_gid(unsigned id) { return id == real_gid ? 0 : id; }

/* The syscalls Android's filter kills in the app domain (docs/reference/device-census.md),
 * and what each is told.  Anything not listed is told ENOSYS, which is also what
 * a program that probes for a newer call expects: glibc falls back for most. */
void policy_deny(unsigned nr, struct deny *d)
{
	d->kind = D_RESULT;
	d->value = -38;                         /* ENOSYS */
	d->to = 0;
	d->flags_arg = -1;
	d->ok_mask = 0;
	d->zero_from = -1;
	switch (nr) {
	case 143: case 144: case 145: case 146: /* setregid setgid setreuid setuid */
	case 149: case 151: case 152: case 159: /* setresgid setfsuid setfsgid setgroups: succeed, as root would */
		d->value = 0;
		break;
	case 202:                               /* accept -> accept4 with no flags: the same call */
		d->kind = D_SUBST; d->to = 242; d->zero_from = 3;
		break;
	case 18: case 39: case 40: case 51:     /* lookup_dcookie umount2 mount chroot */
	case 105: case 106: case 273:           /* init_module delete_module finit_module */
	case 112: case 170: case 171: case 266: /* clock_settime settimeofday adjtimex clock_adjtime */
	case 116: case 161: case 162:           /* syslog sethostname setdomainname */
	case 224: case 225: case 280:           /* swapon swapoff bpf: privileged, so refused as for any non-root user */
		d->value = -1;                  /* EPERM */
		break;
	case 439:                               /* faccessat2 -> faccessat; AT_EACCESS (0x200) is close enough */
		d->kind = D_SUBST; d->to = 48; d->flags_arg = 3; d->ok_mask = 0x200;
		break;
	case 452:                               /* fchmodat2 -> fchmodat; no flag can be honoured */
		d->kind = D_SUBST; d->to = 53; d->flags_arg = 3; d->ok_mask = 0;
		break;
	case 437:                               /* openat2 -> openat */
		d->kind = D_OPENAT2;
		break;
	default:
		break;
	}
}
