/* policy.h -- what the catcher and the applier are told; the only place that
 * knows which syscall means what. */
#ifndef POLICY_H
#define POLICY_H
#include <stddef.h>
#if defined(DN_LIBC) || defined(DN_FREE)
/* The law is lifted out of the tracer into other builds: the shim (lane.c, a
 * glibc library, -DDN_LIBC) and the loader hook (ldhook.c, freestanding,
 * -DDN_FREE).  Both give it their own types, strings, and the one syscall the
 * law makes -- readlink of a host path, through the lane (pol_readlink). */
#include <stdint.h>
#ifdef DN_LIBC
#include <sys/types.h>
#else
typedef long ssize_t;
#endif
typedef uint64_t u64;
typedef int64_t i64;
ssize_t pol_readlink(const char *host, char *buf, size_t n);
/* the law's second syscall: read the owner store (user.dn.owner). */
ssize_t pol_getxattr(const char *host, int nofollow, const char *name, char *buf, size_t n);
#else
#include "sys.h"
#endif

/* One row per syscall to catch.  The monitor builds its filter from the rows
 * and knows nothing else about any syscall. */
struct row {
	unsigned nr;          /* arm64 syscall number */
	unsigned exit_stop;   /* 1: stop again at exit (the result is needed) */
	int path_arg;         /* register index of a path argument, or -1 */
	int path_arg2;        /* a second path argument (rename, link), or -1 */
	int special;          /* SP_*: handled by a dedicated part of the applier */
	int fake;             /* F_*: answer at entry without running the syscall (fake root) */
	int result;           /* R_*: rewrite what the syscall returned, at exit */
	int result_arg;       /* register index of the buffer the result is in (R_STAT, R_STATX) */
	int effect;           /* E_*: what the call may change that the tracer keeps a copy of */
};

/* E_LINKS: the call may make, remove or move a link (or a directory holding one), so
 * the link cache is dropped.  E_CWD: the call changes the working directory. */
enum { E_LINKS = 1, E_CWD = 2 };

/* R_CWD: getcwd's buffer.  R_LINK: readlinkat's buffer, and /proc/<pid>/exe.
 * R_STAT, R_STATX: the owner in a stat buffer.  R_XLIST: listxattr, whose answer
 * drops the SELinux label (a Debian without SELinux shows none).  R_DIRENT:
 * getdents64, filtered to the traced pids for /proc. */
enum { R_NONE = 0, R_CWD = 1, R_LINK = 2, R_STAT = 3, R_STATX = 4, R_XLIST = 5, R_DIRENT = 6 };

/* SP_SOCK_IN / SP_SOCK_OUT: the call's path is inside a sockaddr_un (an AF_UNIX
 * socket), mapped guest<->host like any other path: `path_arg` is the register
 * of the sockaddr (or of a msghdr, when `path_arg2` is -1), `path_arg2` the
 * register of its length (or -1 for a msghdr).  OUT is mapped at exit. */
enum { SP_NONE = 0, SP_EXEC = 1, SP_LINK = 2, SP_SOCK_IN = 3, SP_SOCK_OUT = 4 };
/* F_ZERO: return 0.  F_RESIDS: getresuid/getresgid, write three zero ids.
 * F_GROUPS: getgroups, one group, 0. */
enum { F_NONE = 0, F_ZERO = 1, F_RESIDS = 2, F_GROUPS = 3, F_EXDEV = 4, F_GETID = 5, F_SETID = 6 };
/* F_GETID, F_SETID: the ids the process was last given (fake root keeps them per process). */

size_t policy_count(void);
const struct row *policy_at(size_t i);
const struct row *policy_row(unsigned nr);     /* NULL: not caught */

void policy_set_root(const char *root);        /* the tracer: the root, the names and the cache */
void policy_root_set(const char *root);        /* just the root and the names; no syscall */
const char *policy_root(void);
size_t policy_root_len(void);
/* Is this path the host's -- under the prefix root, or one of the host directories? */
int policy_is_host(const char *p);
/* Point the link cache at the shared generation the caller has mapped (the shim). */
void policy_links_use(volatile unsigned *g);

/* The socket paths that stat must show as S_IFSOCK: the ADB host's abstract
 * sockets have a regular placeholder, so the mode is rewritten.  The table is
 * one mmap'd file the tracer adds to and the shim reads (guest and host paths). */
void policy_sock_init(void);
void policy_sock_add(const char *path);
void policy_sock_del(const char *path);
int policy_sock_has(const char *path);

/* The link cache: what readlink said for a host path, so that a walk costs no
 * syscall for a component already seen.  Its generation lives in a file of the
 * prefix that every tracer on it maps, so a link made under one session drops the
 * cache of all of them; without that file there is no cache. */
void policy_links_changed(void);
unsigned policy_links_gen(void);

/* The binds (the table that maps guest paths to host paths).  The rows a boot
 * starts with are set by policy_set_root/policy_root_set; boot/binds adds more.
 * `policy_binds_add` refuses a duplicate GUEST, a GUEST under a host row, or a
 * non-canonical HOST; `policy_binds_parse` reads the file's text in place. */
int policy_binds_add(const char *guest, const char *host, unsigned flags);
int policy_binds_parse(char *buf);
/* Are both guest paths on the same row (mount)?  A rename or a link between two
 * rows is EXDEV, as between two bind mounts. */
int policy_same_row(const char *g1, const char *g2);
/* Is the row a guest path is in mounted read-only? */
int policy_row_ro(const char *guest);
/* Does this syscall change the tree at a path argument?  (openat is a write only
 * with a write flag, so the caller adds it.) */
int policy_writes(unsigned nr);
/* The mount table from the rows: /proc/self/mounts (mountinfo=0) or mountinfo
 * (mountinfo=1) into `buf`; the length. */
int policy_mounts(char *buf, size_t cap, int mountinfo);
/* The canonical guest path for `in`, as realpath would make it: every component's
 * link followed, `..` against what is resolved, an absolute link target the
 * prefix's.  `base` is the guest working directory for a relative path.  0 and
 * `out` (NUL-terminated), or -errno.  A host path comes back as it is. */
int policy_resolve(const char *base, const char *in, char *out, size_t cap);

/* The law for a path: 0 if `in` stays as it is, else the length of the
 * rewritten path (NUL included) written to `out`. */
int policy_path(const char *in, char *out, size_t cap);
/* The same with symlinks resolved against the prefix (an absolute link target is
 * the prefix's, not the host's); `nofollow`: leave the last component as a link. */
int policy_path_nf(const char *in, char *out, size_t cap, int nofollow);
/* Does the syscall act on the link itself, for the path in register `arg`? */
int policy_nofollow(unsigned nr, int arg, const u64 *regs);
/* A relative path from the host directory `base`; the register of its directory fd. */
int policy_path_at(const char *base, const char *in, char *out, size_t cap, int nofollow);
int policy_dirfd_arg(unsigned nr, int arg);

/* The reverse of policy_path, for what the kernel returns: a host path as the
 * program should see it.  0 if it stays, else the length (NUL included) written
 * to `out`. */
int policy_unpath(const char *host, char *out, size_t cap);

/* Fake root's owner: the real uid and gid of this process read as root. */
void policy_set_ids(unsigned uid, unsigned gid);
unsigned policy_map_uid(unsigned id);
unsigned policy_map_gid(unsigned id);
/* the app's real uid/gid, as the tracer read them at boot */
unsigned policy_real_uid(void);
unsigned policy_real_gid(void);

/* The stored owner of a host path (user.dn.owner "UID:GID", read through the
 * lane); 1 if present, else the caller uses the implicit root.  One copy of the
 * fix-up, shared by the tracer, the shim and dn-exec. */
int policy_own_read(const char *host, int nofollow, unsigned *u, unsigned *g);

/* the shim the exec gate preloads into a glibc program (/usr/lib/deb-native/
 * dn-lane.so, a host path when the prefix has it, else ""): the runtime's own,
 * so it stays host -- the shim finds the root from its own path */
const char *policy_preload(void);
/* the exec gate's userland exec (usr/lib/deb-native/dn-exec, a host path; empty
 * when the prefix ships none); see docs/spec/runtime-revision.md */
const char *policy_dnexec(void);
/* Is this PT_INTERP a glibc loader?  The gate runs it, whatever the tree's own
 * spelling (Debian, Ubuntu, Arch, ...); the path itself is the tree's. */
int policy_is_glibc_interp(const char *interp);

/* The answer to a syscall the platform's own filter killed (it arrives as
 * SIGSYS).  D_RESULT: put `value` in the result register (0 or -errno).
 * D_SUBST: run the equivalent syscall `to` instead, with the arguments as
 * they are (the extra ones are ignored); if `flags_arg` is set and carries a bit outside `ok_mask`, the
 * substitute cannot honour it and the answer is ENOSYS instead (the program's
 * own fallback then does it).  D_OPENAT2: openat2 as openat, from its open_how. */
enum { D_RESULT = 0, D_SUBST = 1, D_OPENAT2 = 2 };
struct deny {
	int kind;
	i64 value;
	unsigned to;
	int flags_arg;        /* register index of the flags argument, or -1 */
	u64 ok_mask;          /* the flag bits the substitute honours */
	int zero_from;        /* D_SUBST: clear the arguments from this register on (an extra one is 0), or -1 */
};
void policy_deny(unsigned nr, struct deny *d);

#endif
