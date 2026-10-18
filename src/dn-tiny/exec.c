/* exec.c -- the exec gate: what to run when the program asks to execve().
 *
 * The kernel cannot start most of what is in the prefix as it stands: a script's
 * `#!/bin/sh` and a glibc ELF's PT_INTERP (/lib/ld-linux-aarch64.so.1) name
 * paths that exist only inside the prefix.  So at execve the gate reads the head
 * of the target and rewrites the call:
 *
 *   script        #!interp [arg]      ->  run interp with the script as its argument
 *   glibc ELF     PT_INTERP = glibc   ->  that loader --argv0 A program args
 *   anything else                     ->  the program, its path mapped
 *
 * The rules are policy (policy_is_glibc_interp, policy_path); this file only
 * reads, decides which rule matches, and builds the new argv.  The loader is the
 * tree's own and finds the libraries through the tree's ld.so.cache.
 */
#include <linux/elf.h>
#include "exec.h"
#include "policy.h"
#include "trace.h"

#define MAXARGS 256
#define DEPTH   4                 /* script -> interpreter script -> ... */

unsigned long exec_n_rewritten;

struct vec {                      /* an argv built in the tracer's memory */
	char *s[MAXARGS + 8];
	int n;
	char pool[32768];
	size_t used;
};

static int vpush(struct vec *v, const char *s)
{
	size_t n = strlen(s) + 1;

	if (v->n >= MAXARGS || v->used + n > sizeof v->pool)
		return -1;
	memcpy(v->pool + v->used, s, n);
	v->s[v->n++] = v->pool + v->used;
	v->used += n;
	return 0;
}

/* a guest path as a host path (the path itself if the policy leaves it) */
static void host_of(const char *guest, char *out, size_t cap)
{
	if (policy_path(guest, out, cap) == 0)
		scpy(out, cap, guest);
}

/* The program's guest path, links followed, for /proc/<pid>/exe: the law resolves
 * it and the root is stripped back off.  A host path (or one the law leaves) stays
 * as it is. */
static void guest_canon(const char *in, char *out, size_t cap)
{
	char host[512];

	if (policy_path_nf(in, host, sizeof host, 0) > 0 && policy_unpath(host, out, cap) > 0)
		return;
	scpy(out, cap, in);
}

static ssize_t head(const char *host, unsigned char *buf, size_t n)
{
	int fd = sys_open(host, 0x80000 /* O_RDONLY | O_CLOEXEC */);
	ssize_t k;

	if (fd < 0)
		return -1;
	k = sys_read(fd, buf, n);
	sys_close(fd);
	return k;
}

/* PT_INTERP of an ELF64 image held in b[0..n); 0 if there is none */
static int elf_interp(const unsigned char *b, ssize_t n, char *out, size_t cap)
{
	const Elf64_Ehdr *e = (const void *) b;
	int i;

	if ((size_t) n < sizeof *e || memcmp(b, ELFMAG, SELFMAG) != 0 || e->e_ident[EI_CLASS] != ELFCLASS64)
		return 0;
	for (i = 0; i < e->e_phnum; i++) {
		size_t off = e->e_phoff + (size_t) i * e->e_phentsize;
		const Elf64_Phdr *p = (const void *) (b + off);

		if (off + sizeof *p > (size_t) n)
			break;
		if (p->p_type == PT_INTERP && p->p_offset + p->p_filesz <= (size_t) n && p->p_filesz < cap) {
			memcpy(out, b + p->p_offset, p->p_filesz);
			out[p->p_filesz] = '\0';
			return 1;
		}
	}
	return 0;
}

/* The original argv as the tracee holds it.  Only argv[0] is read as a string (it can
 * change: a script's interpreter replaces it); the pointers to argv[1..] are kept as they
 * are and put back after whatever is built in front, so the number and the length of the
 * arguments are not limited here. */
#define MAXORIG 65536
static u64 orig_ptr[MAXORIG];
static int orig_n;

static int read_argv(struct tracee *t, u64 argv, struct vec *v)
{
	char s[4096];
	ssize_t m;
	int done = 0;

	orig_n = 0;
	while (!done && orig_n < MAXORIG) {
		int want = MAXORIG - orig_n < 256 ? MAXORIG - orig_n : 256, i;
		ssize_t k = mon_read(t, argv + (u64) orig_n * 8, &orig_ptr[orig_n], (size_t) want * 8);

		if (k < 8)
			return -1;
		for (i = 0; i < (int) (k / 8); i++) {
			if (orig_ptr[orig_n] == 0) {
				done = 1;
				break;
			}
			orig_n++;
		}
	}
	if (!done || orig_n == 0)
		return -1;                              /* no argv[0], or an argv too long to carry */
	m = mon_read_str(t, orig_ptr[0], s, sizeof s - 1);
	if (m <= 0)
		return -1;
	s[m] = '\0';
	return vpush(v, s);
}

/* The setuid bit is the upward (and downward) identity gate: a 4755 binary whose
 * stored owner is root gives the process euid 0 after the exec; one owned by the
 * real uid drops to it.  Compatibility, not privilege: the kernel still sees the
 * app (spec/identity.md). */
static int setuid_identity(struct tracee *t, const char *prog)
{
	char host[512];
	u64 st[18];
	unsigned mode, u, g;

	host_of(prog, host, sizeof host);
	if (sys_fstatat(host, st, 0) != 0)
		return -1;
	mode = *(unsigned *) ((char *) st + 16);
	if (!(mode & 04000))
		return -1;
	if (!trace_stored_owner(host, &u, &g))
		u = 0;                           /* an unmarked setuid file is a package's: root */
	if (u == 0) {
		trace_euid_next(t->pid, 0);
		return 0;
	}
	if (u == policy_real_uid()) {
		trace_euid_next(t->pid, policy_real_uid());
		return (int) policy_real_uid();
	}
	return -1;
}

/* The exec gate tells the new process's shim its six ids in DN_IDS
 * (ruid:euid:suid:rgid:egid:sgid): a hint for the common case, so get*id and a
 * stat's classification need no tracer stop.  A set*id invalidates it. */
#define MAXENVV 512
static u64 env_old[MAXENVV];
static u64 env_new[MAXENVV + 8];

static int read_env(struct tracee *t, u64 envp)
{
	int n = 0;

	if (envp == 0)
		return 0;
	while (n < MAXENVV) {
		u64 p;
		ssize_t k = mon_read(t, envp + (u64) n * 8, &p, 8);

		if (k != 8)
			return -1;
		if (p == 0)
			return n;
		env_old[n++] = p;
	}
	return -1;
}

static int env_is(struct tracee *t, u64 p, const char *name, size_t len)
{
	char b[64];

	if (mon_read(t, p, b, len) != (ssize_t) len)
		return 0;
	return memcmp(b, name, len) == 0;
}

static int put_dec(char *b, int k, unsigned v)
{
	char d[12];
	int j = 0;

	do d[j++] = (char) ('0' + v % 10); while ((v /= 10));
	while (j)
		b[k++] = d[--j];
	return k;
}

static void inject_ids(struct tracee *t, const unsigned id[6], int reg)
{
	char b[64], u[32], g[32];
	int n, i, m = 0, k = 0, ku = 0, kg = 0;
	u64 sp, spu, spg, arr;

	n = read_env(t, t->r.regs[reg]);
	if (n < 0)
		return;
	for (i = 0; i < n; i++)
		if (!env_is(t, env_old[i], "DN_IDS=", 7) &&
		    !env_is(t, env_old[i], "DN_REAL_UID=", 12) &&
		    !env_is(t, env_old[i], "DN_REAL_GID=", 12))
			env_new[m++] = env_old[i];
	memcpy(b, "DN_IDS=", 7);
	k = 7;
	for (i = 0; i < 6; i++) {
		if (i)
			b[k++] = ':';
		k = put_dec(b, k, id[i]);
	}
	b[k] = '\0';
	sp = mon_scratch(t, b, (size_t) k + 1);
	memcpy(u, "DN_REAL_UID=", 12);
	ku = put_dec(u, 12, policy_real_uid());
	u[ku] = '\0';
	spu = mon_scratch(t, u, (size_t) ku + 1);
	memcpy(g, "DN_REAL_GID=", 12);
	kg = put_dec(g, 12, policy_real_gid());
	g[kg] = '\0';
	spg = mon_scratch(t, g, (size_t) kg + 1);
	if (sp == 0 || spu == 0 || spg == 0)
		return;
	env_new[m++] = sp;
	env_new[m++] = spu;
	env_new[m++] = spg;
	env_new[m] = 0;
	arr = mon_scratch(t, env_new, (size_t) (m + 1) * 8);
	if (arr != 0)
		mon_set_arg(t, reg, arr);
}/* Decide the final program and argv.  `cur_in` is the guest path being run; `a`
 * holds argv as it stands (a->s[0] is argv[0]).  Fills `final` (a host path) and
 * `out`. */
static void resolve(const char *cur_in, struct vec *a, char *final, size_t cap, struct vec *out,
		    char *prog, size_t pcap)
{
	char cur[512], host[512], interp[512];
	unsigned char b[1024];
	int depth, i;

	scpy(cur, sizeof cur, cur_in);
	for (depth = 0; depth < DEPTH; depth++) {
		ssize_t n;

		host_of(cur, host, sizeof host);
		n = head(host, b, sizeof b - 1);
		if (n < 2)
			break;

		if (b[0] == '#' && b[1] == '!') {                 /* a script */
			char line[1024], *ip, *arg, *e;
			struct vec na = { .n = 0, .used = 0 };

			b[n] = '\0';
			e = strchr((char *) b, '\n');
			if (e)
				*e = '\0';
			scpy(line, sizeof line, (char *) b + 2);
			for (ip = line; *ip == ' ' || *ip == '\t'; ip++)
				;
			arg = strpbrk(ip, " \t");
			if (arg) {
				*arg++ = '\0';
				while (*arg == ' ' || *arg == '\t')
					arg++;
				if (*arg == '\0')
					arg = NULL;
			}
			if (*ip == '\0')
				break;
			vpush(&na, ip);                           /* argv[0] = the interpreter as written */
			if (arg)
				vpush(&na, arg);
			vpush(&na, cur);                          /* the script, as it was named */
			for (i = 1; i < a->n; i++)
				vpush(&na, a->s[i]);
			*a = na;
			for (i = 0; i < a->n; i++)
				a->s[i] = a->pool + (a->s[i] - na.pool);
			scpy(cur, sizeof cur, ip);
			continue;
		}

		if (elf_interp(b, n, interp, sizeof interp) && policy_is_glibc_interp(interp)) {
			vpush(out, interp);                       /* the tree's own loader */
			if (policy_preload()[0]) {                /* the shim, when the prefix ships one */
				vpush(out, "--preload");
				vpush(out, policy_preload());
			}
			if (policy_ldhook()[0]) {                 /* the loader hook, when the prefix ships one */
				vpush(out, "--audit");
				vpush(out, policy_ldhook());
			}
			vpush(out, "--argv0");
			vpush(out, a->n > 0 ? a->s[0] : cur);
			vpush(out, cur);                          /* the program, as the tree names it */
			for (i = 1; i < a->n; i++)
				vpush(out, a->s[i]);
			host_of(interp, final, cap);              /* the loader, a host path for the kernel */
			guest_canon(cur, prog, pcap);             /* /proc/<pid>/exe says the program */
			return;
		}
		break;                                            /* static, foreign or not an ELF */
	}
	/* run it as it is, with its path mapped */
	for (i = 0; i < a->n; i++)
		vpush(out, a->s[i]);
	host_of(cur, final, cap);
	guest_canon(cur, prog, pcap);
}

/* execve("./prog") and execve("dir/prog") name the program from the cwd, which the
 * gate would not map: make the path absolute (in the guest's spelling) first */
static void absolute_from_cwd(struct tracee *t, char *path, size_t cap)
{
	char link[40] = "/proc/", host[256], cwd[256], full[512], dig[12];
	size_t i = 6, n;
	pid_t pid = t->pid;
	int nd = 0;
	ssize_t r;

	if (path[0] == '/' || path[0] == '\0')
		return;
	do { dig[nd++] = (char) ('0' + pid % 10); pid /= 10; } while (pid);
	while (nd)
		link[i++] = dig[--nd];
	scpy(link + i, sizeof link - i, "/cwd");
	r = sys_readlink(link, host, sizeof host - 1);
	if (r <= 0)
		return;
	host[r] = '\0';
	if (policy_unpath(host, cwd, sizeof cwd) == 0)
		return;                                 /* the cwd is outside the prefix: nothing to map */
	n = strlen(cwd);
	if (n + 1 + strlen(path) + 1 > sizeof full)
		return;
	memcpy(full, cwd, n);
	if (n > 0 && full[n - 1] != '/')
		full[n++] = '/';
	scpy(full + n, sizeof full - n, path);
	scpy(path, cap, full);
}

#define AT_FDCWD_      (-100)
#define AT_EMPTY_PATH_ 0x1000

/* the guest path a file descriptor names (execveat's AT_EMPTY_PATH, or a relative
 * name's directory fd): the length, or <= 0 */
static ssize_t fd_guest(struct tracee *t, int fd, char *out, size_t cap)
{
	char link[48] = "/proc/", host[512], guest[512], dig[12];
	size_t i = 6, n;
	int nd = 0;
	pid_t pid = t->pid;
	unsigned f = (unsigned) fd;
	ssize_t r;

	do { dig[nd++] = (char) ('0' + pid % 10); pid /= 10; } while (pid);
	while (nd) link[i++] = dig[--nd];
	scpy(link + i, sizeof link - i, "/fd/");
	i += 4;
	do { dig[nd++] = (char) ('0' + f % 10); f /= 10; } while (f);
	while (nd) link[i++] = dig[--nd];
	link[i] = '\0';
	r = sys_readlink(link, host, sizeof host - 1);
	if (r <= 0)
		return -1;
	host[r] = '\0';
	n = policy_unpath(host, guest, sizeof guest);
	if (n == 0 || n > cap + 1)
		return -1;
	memcpy(out, guest, n);
	return (ssize_t) n - 1;
}

/* execveat's pathname (x1) relative to its dirfd (x0): the dirfd's guest path and
 * the name; an absolute name, AT_FDCWD, or a dirfd outside the prefix falls back to
 * the cwd */
static void execveat_relative(struct tracee *t, int dirfd, char *path, size_t cap)
{
	char base[512], full[1024];
	size_t n, pl;

	if (path[0] == '/' || dirfd == AT_FDCWD_ || fd_guest(t, dirfd, base, sizeof base) <= 0) {
		absolute_from_cwd(t, path, cap);
		return;
	}
	n = strlen(base);
	pl = strlen(path);
	if (n + 1 + pl + 1 > sizeof full)
		return;
	memcpy(full, base, n);
	if (n > 0 && full[n - 1] != '/')
		full[n++] = '/';
	memcpy(full + n, path, pl + 1);
	scpy(path, cap, full);
}

void exec_enter(struct tracee *t)
{
	struct vec a = { .n = 0, .used = 0 }, out = { .n = 0, .used = 0 };
	static u64 ptrs[MAXORIG + MAXARGS + 8];
	u64 pa, pp;
	char path[512], final[512], prog[512];
	unsigned nr = (unsigned) t->r.regs[8];
	int parg = nr == 281 ? 1 : 0, aarg = nr == 281 ? 2 : 1;
	ssize_t k;
	int i;

	if (nr == 281 && ((unsigned) t->r.regs[4] & AT_EMPTY_PATH_)) {
		k = fd_guest(t, (int) t->r.regs[0], path, sizeof path - 1);
	} else {
		k = mon_read_str(t, t->r.regs[parg], path, sizeof path - 1);
		if (k > 0) {
			path[k] = '\0';
			if (nr == 281)
				execveat_relative(t, (int) t->r.regs[0], path, sizeof path);
			else
				absolute_from_cwd(t, path, sizeof path);
		}
	}
	if (k <= 0)
		return;
	if (read_argv(t, t->r.regs[aarg], &a) < 0 || a.n == 0)
		return;
	resolve(path, &a, final, sizeof final, &out, prog, sizeof prog);
	{
		unsigned id[6];
		int ne = setuid_identity(t, prog);

		trace_ids_of(t->pid, id);
		if (ne >= 0)
			id[1] = id[2] = (unsigned) ne;
		inject_ids(t, id, nr == 281 ? 3 : 2);
	}
	/* what /proc/<pid>/exe will say once the exec succeeds (prog is a guest path) */
	trace_exe_pending(t->pid, prog);

	for (i = 0; i < out.n; i++) {
		ptrs[i] = mon_scratch(t, out.s[i], strlen(out.s[i]) + 1);
		if (ptrs[i] == 0)
			return;
	}
	for (i = 1; i < orig_n; i++)                    /* argv[1..] as they were, still in the tracee */
		ptrs[out.n + i - 1] = orig_ptr[i];
	ptrs[out.n + orig_n - 1] = 0;
	pa = mon_scratch(t, ptrs, (size_t) (out.n + orig_n) * 8);
	pp = mon_scratch(t, final, strlen(final) + 1);
	if (pa == 0 || pp == 0)
		return;
	if (nr == 281) {                                /* execveat(dirfd, path, argv, envp, flags) */
		mon_set_arg(t, 0, (u64) -100);          /* AT_FDCWD */
		mon_set_arg(t, 1, pp);
		mon_set_arg(t, 2, pa);
		mon_set_arg(t, 4, 0);
	} else {
		mon_set_arg(t, 0, pp);
		mon_set_arg(t, 1, pa);
	}
	exec_n_rewritten++;
}
