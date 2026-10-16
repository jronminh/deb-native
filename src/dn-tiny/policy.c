/* policy.c -- the table and the law, in their smallest form. */
#include "policy.h"

#ifdef DN_LIBC
#include <string.h>

/* the two helpers sys.c gives the tracer, here from libc */

static int scpy(char *out, size_t cap, const char *s)
{
	size_t n = strlen(s);

	if (n + 1 > cap)
		return -1;
	memcpy(out, s, n + 1);
	return (int) n;
}
#elif defined(DN_FREE)
/* Freestanding (ldhook.c): the law carries the few string helpers it needs, so
 * the loader hook links no libc (it loads before the loader opens libc). */
static size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char) c) return (char *) s; return c == 0 ? (char *) s : 0; }
static int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char) *a - (unsigned char) *b; }
static int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char) *a - (unsigned char) *b : 0; }
static void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
static void *memmove(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; if (a < b) while (n--) *a++ = *b++; else while (n--) a[n] = b[n]; return d; }
static int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }
static int scpy(char *out, size_t cap, const char *s)
{
	size_t n = strlen(s);

	if (n + 1 > cap)
		return -1;
	memcpy(out, s, n + 1);
	return (int) n;
}
#else
#define pol_readlink sys_readlink
#endif

/* the working-path buffer the law walks in, and its own errno values (so it
 * needs no errno.h) */
#define WORK 4096
enum { E_NOENT = 2, E_INVAL = 22, E_NAMETOOLONG = 36, E_LOOP = 40 };

static const struct row ROWS[] = {
#include "policy-rows.inc"        /* generated: the caught syscalls and what is done with each */
};
#define NROWS (sizeof ROWS / sizeof ROWS[0])

static const struct row *by_nr[512];
static int indexed;

static const char *root;
static size_t root_len;
static char loader[512], libpath[1024], preload[512], ldhook[512];

/* ---- the link cache (policy.h).  An entry is valid while its generation is the
 * shared one; moving the generation drops every entry at once.  A seqlock lets
 * the shim's threads share it (the tracer is single-threaded and pays nothing). -- */
#define LC_SLOTS 4096
#define LC_KEY   192
#define LC_TGT   128
struct lc {
	unsigned seq;                 /* odd: a writer holds the entry */
	unsigned gen;                 /* 0: empty */
	unsigned hash;
	int r;                        /* what readlink returned: <= 0 not a link, else the target's length */
	char key[LC_KEY];
	char tgt[LC_TGT];
};
static struct lc lcache[LC_SLOTS];
static volatile unsigned *links_gen;  /* in the shared file; NULL: no cache */

#if !defined(DN_LIBC) && !defined(DN_FREE)
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
#endif

void policy_links_use(volatile unsigned *g) { links_gen = g; }

/* one atomic increment, inline so the tracer links no libatomic */
static void add1(unsigned *p)
{
	unsigned v, fail;

	__asm__ volatile("1: ldaxr %w0, [%2]\n"
			 "   add   %w0, %w0, #1\n"
			 "   stlxr %w1, %w0, [%2]\n"
			 "   cbnz  %w1, 1b\n"
			 : "=&r"(v), "=&r"(fail) : "r"(p) : "memory");
}

void policy_links_changed(void)
{
	if (links_gen)
		add1((unsigned *) links_gen);
}

unsigned policy_links_gen(void) { return links_gen ? *links_gen : 0; }

/* the seqlock an entry is read and written under (lane.c's, for the same reason:
 * a thread must never read a key or a target while another writes it). ---------- */
static int pseq_read(unsigned *s, unsigned *v) { *v = __atomic_load_n(s, __ATOMIC_ACQUIRE); return !(*v & 1); }
static int pseq_begin(unsigned *s)
{
	unsigned v, nv, fail, busy;

	__asm__ volatile("1: ldaxr %w0, [%4]\n"
			 "   and   %w3, %w0, #1\n"
			 "   cbnz  %w3, 2f\n"
			 "   add   %w1, %w0, #1\n"
			 "   stlxr %w2, %w1, [%4]\n"
			 "   cbnz  %w2, 1b\n"
			 "2:"
			 : "=&r"(v), "=&r"(nv), "=&r"(fail), "=&r"(busy) : "r"(s) : "memory", "cc");
	return busy ? -1 : 0;
}
static void pseq_end(unsigned *s) { add1(s); }
static int pseq_check(unsigned *s, unsigned v)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(s, __ATOMIC_RELAXED) == v;
}

static int lc_get(struct lc *e, unsigned cur, unsigned h, const char *k, size_t len, char *buf, size_t n, ssize_t *out)
{
	unsigned s;
	int r;

	if (!pseq_read(&e->seq, &s) || e->gen != cur || e->hash != h || len >= sizeof e->key ||
	    memcmp(e->key, k, len + 1) != 0)
		return 0;
	r = e->r;
	if (r > (int) sizeof e->tgt || (r > 0 && (size_t) r > n))
		return 0;
	if (r > 0)
		memcpy(buf, e->tgt, (size_t) r);
	if (!pseq_check(&e->seq, s))
		return 0;
	*out = r;
	return 1;
}

static void lc_put(struct lc *e, unsigned cur, unsigned h, const char *k, size_t len, const char *buf, ssize_t r)
{
	if (len >= sizeof e->key || r >= (ssize_t) sizeof e->tgt || pseq_begin(&e->seq) < 0)
		return;
	e->gen = cur;
	e->hash = h;
	e->r = (int) r;
	memcpy(e->key, k, len + 1);
	if (r > 0)
		memcpy(e->tgt, buf, (size_t) r);
	pseq_end(&e->seq);
}

/* readlink(host path), from the cache when it holds the answer */
static ssize_t lc_readlink(const char *p, char *buf, size_t n)
{
	unsigned h = 2166136261u, cur;
	size_t len = 0;
	struct lc *e;
	ssize_t r;

	if (!links_gen)
		return pol_readlink(p, buf, n);
	while (p[len]) {
		h = (h ^ (unsigned char) p[len]) * 16777619u;
		len++;
	}
	cur = *links_gen;
	e = &lcache[h % LC_SLOTS];
	if (!lc_get(e, cur, h, p, len, buf, n, &r)) {
		r = pol_readlink(p, buf, n);
		lc_put(e, cur, h, p, len, buf, r);
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

/* ---- the binds: the one table that maps a guest path to a host path.  `/` is
 * the prefix root, and each directory the tree keeps (or a bind the host wrote in
 * boot/binds) is a row.  Guest to host by the longest guest row, host to guest by
 * the longest host row. ------------------------------------------------------- */
#define MAXBINDS 32
#define BIND_G   256
#define BIND_H   512
struct bind {
	char g[BIND_G], h[BIND_H];
	unsigned gl, hl, flags;
};
static struct bind binds[MAXBINDS];
static int nbinds;

/* `pre` is a prefix of `p` at a component boundary */
static int under(const char *p, const char *pre, size_t pl)
{
	if (pl == 0 || strncmp(p, pre, pl) != 0)
		return 0;
	if (pre[pl - 1] == '/')
		return 1;                              /* a prefix ending in '/' is already at a boundary */
	return p[pl] == '/' || p[pl] == '\0';
}

static int binds_add(const char *g, const char *h, unsigned flags)
{
	struct bind *b;

	if (nbinds >= MAXBINDS)
		return -1;
	b = &binds[nbinds];
	if (scpy(b->g, sizeof b->g, g) < 0 || scpy(b->h, sizeof b->h, h) < 0)
		return -1;
	b->gl = (unsigned) strlen(b->g);
	b->hl = (unsigned) strlen(b->h);
	b->flags = flags;
	nbinds++;
	return 0;
}

static const struct bind *row_guest(const char *g)
{
	const struct bind *best = NULL;
	int i;

	for (i = 0; i < nbinds; i++)
		if (under(g, binds[i].g, binds[i].gl) && (!best || binds[i].gl > best->gl))
			best = &binds[i];
	return best;
}

static const struct bind *row_host(const char *h)
{
	const struct bind *best = NULL;
	int i;

	for (i = 0; i < nbinds; i++)
		if (under(h, binds[i].h, binds[i].hl) && (!best || binds[i].hl > best->hl))
			best = &binds[i];
	return best;
}

static int row_is_identity(const struct bind *b)
{
	return b && b->gl == b->hl && strncmp(b->g, b->h, b->gl) == 0;
}

/* a guest path under a host row: it stays the host's, told as it is */
static int is_host_row(const char *g)
{
	return row_is_identity(row_guest(g));
}

/* a, then b, with one '/' where they meet (a '/' row's remainder keeps its own) */
static int join(char *out, size_t cap, const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);

	if (la && a[la - 1] == '/' && b[0] == '/')
		la--;
	if (la + lb + 1 > cap)
		return -1;
	memcpy(out, a, la);
	memcpy(out + la, b, lb + 1);
	return (int) (la + lb);
}

/* where the remainder of `p` starts past prefix `pre` (keeping a leading '/' when
 * `pre` ends with one) */
static const char *rest_after(const char *p, const char *pre, size_t pl)
{
	return p + pl - (pl >= 1 && pre[pl - 1] == '/' ? 1 : 0);
}

/* the host path of guest path g, into out; 0, or -1 (too long) */
static int to_host(const char *g, char *out, size_t cap)
{
	const struct bind *b = row_guest(g);

	if (!b)
		return scpy(out, cap, g) < 0 ? -1 : 0;
	if (strlen(g) == b->gl)
		return scpy(out, cap, b->h) < 0 ? -1 : 0;
	return join(out, cap, b->h, rest_after(g, b->g, b->gl)) < 0 ? -1 : 0;
}

/* the guest path of host path h; 1 if it differs, 0 if not under a row or identity */
static int to_guest(const char *h, char *out, size_t cap)
{
	const struct bind *b = row_host(h);

	if (!b)
		return 0;
	if (strlen(h) == b->hl) {
		if (scpy(out, cap, b->g) < 0)
			return 0;
	} else if (join(out, cap, b->g, rest_after(h, b->h, b->hl)) < 0) {
		return 0;
	}
	return strcmp(out, h) != 0;
}

/* the rows a boot starts with: `/` is the root, and the host directories under
 * their own names (a "keep" list; boot/binds adds more) */
static void binds_defaults(void)
{
	static const char *const keep[] = { "/proc", "/sys", "/dev", "/data", "/system", "/storage",
					    "/sdcard", "/apex", "/vendor", "/mnt", NULL };
	int i;

	nbinds = 0;
	binds_add("/", root_len ? root : "/", 0);
	for (i = 0; keep[i]; i++)
		binds_add(keep[i], keep[i], 0);
}

/* add one bind from boot/binds; 0, or -1 (bad: a duplicate GUEST, a GUEST under a
 * host row, a non-canonical HOST, a reserved name) */
/* Are both guest paths on the same mount (row)?  A rename or a link between two
 * rows is EXDEV, as between two bind mounts. */
int policy_same_row(const char *g1, const char *g2)
{
	const struct bind *b1 = row_guest(g1), *b2 = row_guest(g2);

	return b1 && b1 == b2;
}

int policy_binds_add(const char *g, const char *h, unsigned flags)
{
	size_t n = strlen(h), i;

	if (g[0] != '/' || h[0] != '/' || g[1] == '\0')
		return -1;                              /* the root is not a bind */
	if (n > 1 && h[n - 1] == '/')
		return -1;
	{                                               /* HOST must be canonical: no empty, "." or ".." component */
		const char *p = h + 1;

		while (*p) {
			const char *c = p;
			size_t cl;

			while (*p && *p != '/')
				p++;
			cl = (size_t) (p - c);
			if (cl == 0 || (cl == 1 && c[0] == '.') || (cl == 2 && c[0] == '.' && c[1] == '.'))
				return -1;
			if (*p == '/')
				p++;
		}
	}
	for (i = 0; i < (size_t) nbinds; i++)
		if (strcmp(binds[i].g, g) == 0)
			return -1;                      /* a duplicate GUEST */
	if (is_host_row(g))
		return -1;                              /* a GUEST under a host directory */
	return binds_add(g, h, flags);
}

/* parse boot/binds text (in place): one line per bind, GUEST<TAB>HOST<TAB>FLAGS.
 * 0, or -1 on a bad line. */
int policy_binds_parse(char *buf)
{
	char *line = buf;

	while (line && *line) {
		char *nl = strchr(line, '\n');
		char *g, *h, *f = NULL;

		if (nl)
			*nl = '\0';
		if (*line && *line != '#') {
			g = line;
			h = strchr(g, '\t');
			if (!h)
				return -1;
			*h++ = '\0';
			f = strchr(h, '\t');
			if (f)
				*f++ = '\0';
			if (policy_binds_add(g, h, f && strcmp(f, "ro") == 0 ? 1 : 0) < 0)
				return -1;
		}
		line = nl ? nl + 1 : NULL;
	}
	return 0;
}

void policy_root_set(const char *r)
{
	root = r;
	root_len = strlen(r);
	binds_defaults();
	loader[0] = libpath[0] = preload[0] = ldhook[0] = '\0';
	append(loader, sizeof loader, "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1");
	append(libpath, sizeof libpath, "/usr/lib/aarch64-linux-gnu:/usr/lib");
	/* the shim and the loader hook are the runtime's own, and the shim finds the
	 * root from its own host path: both stay host paths */
	append(preload, sizeof preload, r);
	append(preload, sizeof preload, "/usr/lib/deb-native/dn-lane.so");
	append(ldhook, sizeof ldhook, r);
	append(ldhook, sizeof ldhook, "/usr/lib/deb-native/dn-ldhook.so");
}

const char *policy_root(void) { return root; }
size_t policy_root_len(void) { return root_len; }

#if !defined(DN_LIBC) && !defined(DN_FREE)
void policy_set_root(const char *r)
{
	char st[128], p[600], buf[4096];
	int fd, i;
	ssize_t k;

	policy_root_set(r);
	if (sys_fstatat(preload, st, 0) != 0)
		preload[0] = '\0';
	if (sys_fstatat(ldhook, st, 0) != 0)
		ldhook[0] = '\0';
	/* the binds the host wrote before boot; a bad line refuses the boot */
	if (cat2(p, sizeof p, r, "/boot/binds") > 0 && (fd = sys_open(p, 0)) >= 0) {
		k = sys_read(fd, buf, sizeof buf - 1);
		sys_close(fd);
		if (k > 0) {
			buf[k] = '\0';
			if (policy_binds_parse(buf) < 0) {
				put(2, "dn-tiny: bad boot/binds\n");
				sys_exit(1);
			}
		}
	}
	for (i = 0; i < nbinds; i++) {           /* an empty directory at each bind's GUEST */
		char b[WORK];
		size_t j;

		if (strcmp(binds[i].g, "/") == 0 || row_is_identity(&binds[i]))
			continue;
		if (cat2(b, sizeof b, r, binds[i].g) < 0)
			continue;
		for (j = root_len; b[j]; j++) {
			if (b[j] != '/')
				continue;
			b[j] = '\0';
			sc(34, -100, (i64) b, 0755, 0, 0, 0);   /* mkdirat, AT_FDCWD */
			b[j] = '/';
		}
		sc(34, -100, (i64) b, 0755, 0, 0, 0);
	}
	links_init();
}
#endif

const char *policy_preload(void) { return preload; }
const char *policy_ldhook(void) { return ldhook; }
const char *policy_loader(void) { return loader; }
const char *policy_libpath(void) { return libpath; }

int policy_is_glibc_interp(const char *interp)
{
	size_t n = strlen(interp), m = sizeof "ld-linux-aarch64.so.1" - 1;

	return n >= m && strcmp(interp + n - m, "ld-linux-aarch64.so.1") == 0;
}

/* what is already a host path stays: under the prefix root (a host spelling), or
 * under one of the host directories (a row that maps to itself) */
int policy_is_host(const char *p)
{
	if (under(p, root, root_len))
		return 1;
	return is_host_row(p);
}

/* ---- symlinks, in the guest's frame.  The kernel would follow an absolute target
 * from the host's /, not the tree's, so the path is walked here one component at a
 * time on the *guest* path (so `..` and links are the tree's, as under a real bind
 * mount), and its host path is taken at the end through the table. -------------- */
#define MAX_HOPS 40

/* the part of guest path `cur` before its last '/', cut in place (never above "/") */
static void pop_component(char *cur)
{
	size_t n = strlen(cur);

	while (n > 0 && cur[n - 1] != '/')
		n--;
	if (n > 0)
		n--;
	cur[n] = '\0';
}

/* the host path for guest path `in` (relative to guest directory `base`), links
 * resolved in guest space; 0 if it cannot be mapped.  The last component is
 * followed unless `nofollow`.  It is the length (NUL included) written to `out`. */
static int walk(const char *base, const char *in, char *out, size_t cap, int nofollow)
{
	char res[WORK], rest[WORK], tgt[WORK], host[WORK], *p;
	size_t rl;
	int hops = 0;

	if (in[0] == '/') {
		res[0] = '\0';
	} else if (scpy(res, sizeof res, base) < 0) {
		return 0;
	} else if (strcmp(res, "/") == 0) {
		res[0] = '\0';
	}
	rl = strlen(res);
	if (scpy(rest, sizeof rest, in) < 0)
		return 0;
	if (in[0] && in[strlen(in) - 1] == '/')
		nofollow = 0;                                  /* a trailing slash follows the link, as the kernel does */
	p = rest;
	for (;;) {
		char comp[WORK], after[WORK], *q, *afterp;
		size_t cl;
		ssize_t r;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		q = p;
		while (*q && *q != '/')
			q++;
		cl = (size_t) (q - p);
		if (cl >= sizeof comp)
			break;
		memcpy(comp, p, cl);
		comp[cl] = '\0';
		afterp = q;
		while (*afterp == '/')
			afterp++;
		if (scpy(after, sizeof after, afterp) < 0)
			break;
		p = q;
		if (cl == 1 && comp[0] == '.')
			continue;
		if (cl == 2 && comp[0] == '.' && comp[1] == '.') {
			pop_component(res);
			rl = strlen(res);
			continue;
		}
		if (rl + 1 + cl + 1 > sizeof res)
			break;                                 /* too long: give up, keep */
		res[rl] = '/';
		memcpy(res + rl + 1, comp, cl);
		res[rl + 1 + cl] = '\0';
		if (nofollow && after[0] == '\0') {
			rl += 1 + cl;
			continue;                              /* a name to create, remove or read as a link */
		}
		if (to_host(res, host, sizeof host) < 0)
			break;
		r = lc_readlink(host, tgt, sizeof tgt - 1);
		if (r <= 0) {
			rl += 1 + cl;
			continue;                              /* not a link (or not there): it stays */
		}
		tgt[r] = '\0';
		if (++hops > MAX_HOPS) {
			rl += 1 + cl;
			break;                                 /* the kernel will say ELOOP */
		}
		{                                              /* the target, then what was left of the path */
			size_t tl = (size_t) r, al = strlen(after);

			if (tl + 1 + al + 1 > sizeof rest)
				break;
			memmove(rest + tl + 1, after, al + 1);
			memcpy(rest, tgt, tl);
			rest[tl] = '/';
			p = rest;
		}
		if (tgt[0] == '/') {
			if (under(tgt, root, root_len)) {
				/* a host-spelled path inside the prefix: back to the guest's */
				memmove(rest, rest + root_len, strlen(rest + root_len) + 1);
				p = rest;
				res[0] = '\0';
			} else if (is_host_row(tgt)) {
				/* a link to a host path: the result leaves the tree */
				return scpy(out, cap, rest) >= 0 ? (int) strlen(out) + 1 : 0;
			} else {
				res[0] = '\0';                 /* an absolute target: from the tree's root */
			}
			rl = 0;
		} else {
			res[rl] = '\0';                        /* back to the directory holding the link */
		}
	}
	if (res[0] == '\0')
		scpy(res, sizeof res, "/");            /* "" is the tree's root */
	if (to_host(res, out, cap) < 0)
		return 0;
	if (in[0] && in[strlen(in) - 1] == '/' && strlen(out) + 2 <= cap)
		memcpy(out + strlen(out), "/", 2);
	return (int) strlen(out) + 1;
}

int policy_path_nf(const char *in, char *out, size_t cap, int nofollow)
{
	if (in[0] != '/' || policy_is_host(in))
		return 0;
	return walk("/", in, out, cap, nofollow);
}

/* a relative path, from a directory the kernel names (a host path): its guest path
 * is found in the table and the name is walked from there; 0 if that directory is
 * a host one (its paths stay) or is outside every row */
int policy_path_at(const char *base, const char *in, char *out, size_t cap, int nofollow)
{
	char gbase[WORK];
	size_t n = strlen(base);
	const struct bind *b;

	if (in[0] == '/' || n == 0 || base[n - 1] == '/')
		return 0;
	b = row_host(base);                     /* base is a host path: classify it as one */
	if (!b || row_is_identity(b))
		return 0;                       /* a host directory: its paths stay */
	if (!to_guest(base, gbase, sizeof gbase))
		return 0;
	return walk(gbase, in, out, cap, nofollow);
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

/* the guest spelling of host path `host`: 0 if it is not under a row, or is the
 * same string (a host row), where the caller keeps it as it is */
int policy_unpath(const char *host, char *out, size_t cap)
{
	if (!to_guest(host, out, cap))
		return 0;
	return (int) strlen(out) + 1;
}

/* readlink of a guest path: on the host path, from the shared cache, an absolute
 * link target inside the prefix told as the guest's. */
static int guest_readlink(const char *g, char *buf, size_t n)
{
	char host[WORK];
	ssize_t r;

	if (to_host(g, host, sizeof host) < 0)
		return -E_NAMETOOLONG;
	r = lc_readlink(host, buf, n);
	if (r > 0 && (size_t) r >= root_len && strncmp(buf, root, root_len) == 0 &&
	    ((size_t) r == root_len || buf[root_len] == '/')) {
		memmove(buf, buf + root_len, (size_t) r - root_len);
		r -= (long) root_len;
		if (r == 0)
			buf[r++] = '/';
	}
	return (int) r;
}

/* The canonical guest path, as realpath would make it: every component's link
 * followed (an absolute target from the prefix root again), `..` against what is
 * resolved so far.  0 and `out`, or -errno.  A missing component is ENOENT. */
int policy_resolve(const char *base, const char *in, char *out, size_t cap)
{
	char rest[WORK], tgt[WORK];
	size_t rl;
	int links = 0;
	char *p;

	if (in[0] == '\0')
		return -E_NOENT;
	if (strlen(in) >= sizeof rest)
		return -E_NAMETOOLONG;
	if (in[0] == '/') {
		out[0] = '\0';
	} else if (scpy(out, cap, base) < 0) {
		return -E_NAMETOOLONG;
	} else if (strcmp(out, "/") == 0) {
		out[0] = '\0';
	}
	rl = strlen(out);
	if (scpy(rest, sizeof rest, in) < 0)
		return -E_NAMETOOLONG;
	p = rest;
	for (;;) {
		char *comp;
		size_t cl;
		int r;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		comp = p;
		while (*p && *p != '/')
			p++;
		cl = (size_t) (p - comp);
		if (cl == 1 && comp[0] == '.')
			continue;
		if (cl == 2 && comp[0] == '.' && comp[1] == '.') {
			while (rl > 0 && out[rl - 1] != '/')
				rl--;
			if (rl > 0)
				rl--;
			out[rl] = '\0';
			continue;
		}
		if (rl + 1 + cl >= cap)
			return -E_NAMETOOLONG;
		out[rl] = '/';
		memcpy(out + rl + 1, comp, cl);
		out[rl + 1 + cl] = '\0';
		r = guest_readlink(out, tgt, sizeof tgt - 1);
		if (r == -E_INVAL) {                    /* there, and not a link */
			rl += 1 + cl;
			continue;
		}
		if (r < 0)
			return r;
		if (++links > MAX_HOPS)
			return -E_LOOP;
		tgt[r] = '\0';
		{
			size_t al = strlen(p);          /* the target, then what was left of the path */

			if ((size_t) r + 1 + al >= sizeof rest)
				return -E_NAMETOOLONG;
			memmove(rest + r + 1, p, al + 1);
			memcpy(rest, tgt, (size_t) r);
			rest[r] = '/';
			p = rest;
		}
		if (tgt[0] == '/')
			rl = 0;                         /* an absolute target: from the root again */
		out[rl] = '\0';
	}
	if (rl == 0) {
		out[0] = '/';
		out[1] = '\0';
	}
	return 0;
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
