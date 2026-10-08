/* dn-policy -- the one place path-remapping, fake-root and hardlink rules
 * live (docs/spec/runtime.md, "dn-policy"). A static C library, linked
 * into both dn-glibc (the fast path, in-process) and dn-trace (the
 * fallback path, via ptrace) so the two can never disagree -- principle 3
 * in runtime.md.
 *
 * Calling convention, settled here before either caller is written:
 *
 * - Plain C ABI, no exceptions. Every function returns 0 on success or a
 *   negative errno on failure, matching the rest of this codebase
 *   (translate_path() et al. in src/tracer/) and the kernel's own
 *   convention.
 *
 * - No library-owned heap crosses the API. Every out-param is a
 *   caller-supplied buffer with a caller-supplied capacity (PATH_MAX is
 *   enough for every path here). dn-glibc uses glibc's own allocator;
 *   dn-trace's codebase uses talloc; dn-policy must not hand either side
 *   a pointer it has to free with the other's allocator. Internally,
 *   dn-policy may allocate (e.g. for the symlink-resolution cache) but
 *   never returns that memory to the caller.
 *
 * - Two different invocation contexts, same function bodies:
 *     - dn-glibc calls these directly, in-process: the path it has is
 *       already in its own address space.
 *     - dn-trace calls these in *its own* process, on *its own* copy of
 *       the tracee's data (read out with process_vm_readv() before the
 *       call, written back with process_vm_writev() or PTRACE_POKEDATA
 *       after, or into registers for a return value). dn-policy itself
 *       never touches another process's memory -- that marshalling is
 *       dn-trace's wrapper, not dn-policy's job. This is also why path
 *       resolution issues real syscalls (stat, readlink) directly rather
 *       than taking them as parameters: both dn-glibc and dn-trace are
 *       ordinary processes with real filesystem access, so dn-policy can
 *       just call the kernel itself instead of asking the caller to.
 *
 * - Thread safety: dn-glibc is multi-threaded, so every function here
 *   must tolerate concurrent calls from multiple threads of the *same*
 *   process. The symlink-resolution cache and the owner-store's on-disk
 *   fallback need their own internal locking (per runtime.md: a file lock
 *   for the on-disk store, since multiple *processes* -- not just
 *   threads -- write it). No function here holds a lock across a
 *   syscall that can block indefinitely.
 *
 * - Idempotent translation: calling dn_policy_translate_path() on an
 *   already-in-tree path is a no-op (runtime.md: "Dịch không được lặp").
 *   Safe to call more than once on the same input.
 *
 * - Nothing here is prefix-specific (principle 5, runtime.md): dn_policy_init()
 *   takes TREE/RT as plain strings the *caller* already derived (dn-glibc
 *   from its own loader path, as the shipped self-derivation already
 *   does; dn-trace from wherever it gets its own equivalent, still
 *   open -- see below). dn-policy itself never hardcodes a path.
 */

#ifndef DN_POLICY_H
#define DN_POLICY_H

#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <linux/limits.h> /* PATH_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Setup -------------------------------------------------------- */

/* Must be called once per process before any other dn_policy_*()
 * function. @tree_root and @rt_root are absolute, caller-derived (see
 * the file comment above) and are copied internally -- the caller's
 * buffers need not outlive this call.
 *
 * Probes the device for what dn_policy_owner_set() can use (the
 * "user.dn.*" xattr vs. the RT/state/ DB fallback, runtime.md's "Fake
 * root"/"The owner store") and records the choice for the rest of the
 * process's lifetime. Returns 0, or a negative errno if @tree_root/
 * @rt_root are unusable (not absolute, too long, ...).
 *
 * DN_POLICY_OWNER_BACKEND=db|xattr in the environment skips the probe
 * and forces that backend (diagnosis, and testing the DB backend's code
 * path on a device where xattr happens to work everywhere TREE could
 * live).
 */
int dn_policy_init(const char *tree_root, const char *rt_root);

/* ---- Path rewriting ------------------------------------------------ */

/* Translate @guest_path (as a program in the tree sees it) into
 * @host_path_out (the real, on-device path), up to @cap bytes including
 * the NUL. Longest-prefix mapping, /proc "/sys /dev passthrough, and
 * absolute in-tree symlink resolution are all applied here (runtime.md,
 * "Path rewriting"). A relative @guest_path is rejected with -EINVAL --
 * resolving a relative path is the kernel's job (via the real cwd),
 * callers never need to ask dn-policy for that; the one exception
 * (blocking ".." above TREE's root) is the caller's responsibility to
 * check against the resolved absolute path, not dn-policy's.
 *
 * Idempotent: a @guest_path already under TREE or RT comes back
 * unchanged (mod NUL-termination).
 *
 * Returns 0 on success, or a negative errno:
 *   -ENAMETOOLONG  the result doesn't fit in @cap
 *   -ELOOP         a symlink chain inside the tree is too deep
 *   -EINVAL        @guest_path is not absolute
 */
int dn_policy_translate_path(const char *guest_path, char *host_path_out, size_t cap);

/* The reverse of dn_policy_translate_path(): turn a real, on-device
 * @host_path back into the guest path a program in the tree should see
 * (runtime.md: getcwd(), /proc/self/{cwd,fd/N,exe}). @host_path need not
 * already be canonical.
 *
 * Returns 0 on success, or a negative errno:
 *   -ENAMETOOLONG  the result doesn't fit in @cap
 *   -ENOENT        @host_path is not under TREE or RT at all
 */
int dn_policy_detranslate_path(const char *host_path, char *guest_path_out, size_t cap);

/* O_NOFOLLOW / AT_SYMLINK_NOFOLLOW: callers that must not resolve the
 * final path component call this instead of dn_policy_translate_path(),
 * which otherwise resolves every component including the last. */
int dn_policy_translate_path_nofollow(const char *guest_path, char *host_path_out, size_t cap);

/* ---- Fake root ------------------------------------------------------ */

/* get*id()/set*id() family: trivial, no real privilege involved -- but
 * the current fake uid/gid is state *per traced program*, not a global
 * inside dn-policy. dn-glibc runs in-process, so it only ever has one
 * program to track and can keep a single static DnIdentity of its own.
 * dn-trace tracks many tracees at once (runtime.md : "apt hạ quyền xuống
 * _apt" while another tracee stays root), so it must keep one DnIdentity
 * per Tracee it already has a struct for, not share one across all of
 * them -- a global here would let one tracee's setuid() leak into
 * another's getuid().
 *
 * DnIdentity is an opaque handle dn-policy allocates and frees; it is
 * the one exception to "no library-owned heap crosses the API" above,
 * since the pair of calls below is matched and the caller never calls
 * free() on it directly. */
typedef struct DnIdentity DnIdentity;

DnIdentity *dn_policy_identity_new(void);
void dn_policy_identity_free(DnIdentity *id);

uid_t dn_policy_fake_getuid(const DnIdentity *id);
gid_t dn_policy_fake_getgid(const DnIdentity *id);
/* set*id(): records the value and always succeeds. */
void dn_policy_fake_setuid(DnIdentity *id, uid_t uid);
void dn_policy_fake_setgid(DnIdentity *id, gid_t gid);

/* The owner-store record for one file, identified by (st_dev, st_ino) so
 * it survives a rename. All fields are what runtime.md's "Fake root"
 * fakes: chown/fchown/fchownat's target, chmod's setuid/setgid bits, and
 * mknod's device type. */
typedef struct {
	uid_t  uid;
	gid_t  gid;
	mode_t mode_bits;  /* only the setuid/setgid/device-type bits this
			    * store owns; never the full st_mode */
	dev_t  rdev;       /* for a faked device file (mknod) */
} DnOwnerRecord;

/* Look up the owner-store record for @host_path (identified by
 * (@dev, @ino) so a rename is still the same record under the DB
 * backend; the xattr backend ignores dev/ino and reads @host_path's
 * own xattr directly -- both are needed because the two backends key
 * the record differently, not because the caller should pick one).
 * Returns 0 with *@out filled in, or -ENOENT if the file has no record
 * (caller should then report uid 0, gid 0 -- "an ordinary Debian
 * install", per runtime.md), or another negative errno on a real
 * failure reading the store. */
int dn_policy_owner_get(const char *host_path, dev_t dev, ino_t ino, DnOwnerRecord *out);

/* Record a new owner for @host_path, creating the entry if absent.
 * Returns 0, or a negative errno (-EIO for the on-disk backend, -ENOSPC
 * if the xattr backend's value is rejected by the filesystem, ...). */
int dn_policy_owner_set(const char *host_path, dev_t dev, ino_t ino, const DnOwnerRecord *record);

/* Called when a file's last link is removed (runtime.md: "mục bị xóa khi
 * file bị xóa link cuối cùng"). Not an error if there was no record.
 * @host_path may already be gone (the unlink that triggered this call
 * already happened) -- only the DB backend needs it to still exist;
 * the xattr backend's record disappears with the file on its own, so
 * this is a no-op there. */
int dn_policy_owner_forget(const char *host_path, dev_t dev, ino_t ino);

/* Rewrite @st's st_uid/st_gid/permission bits from the owner store
 * before a stat-family result is returned to the guest program.
 * @host_path is the path the real stat/fstatat/statx call just used --
 * needed here for the same reason dn_policy_owner_get() needs it (the
 * xattr backend has no other way to find the record); @st already
 * carries (st_dev, st_ino) for the DB backend's key. A file absent from
 * the store is left reporting uid 0, gid 0. Returns 0 always (a
 * store-read failure here degrades to "no record", not an error --
 * breaking every stat() call over a store hiccup is worse than
 * occasionally under-reporting an owner). */
void dn_policy_fake_stat(const char *host_path, struct stat *st);

/* Record a chown()/chmod() the way fake root must: update only the fields
 * that call is about, keeping whatever the existing record already holds
 * (a chown must not wipe chmod's setuid bits, and vice versa).  With no
 * existing record the missing fields default to the "ordinary Debian
 * install" values (uid/gid 0, no special bits).  @dev/@ino identify the
 * file, as for dn_policy_owner_set().  Returns 0 or a negative errno. */
int dn_policy_owner_merge(const char *host_path, uint64_t dev, uint64_t ino,
			  int set_ids, uint32_t uid, uint32_t gid,
			  int set_mode, uint32_t mode_bits);

/* security.* xattrs (setcap and friends): always faked, always
 * succeeds -- a *real* write of one of these is what's failing in the
 * first place (Android denies it), so this never attempts the real
 * xattr regardless of which owner-store backend is active; it always
 * goes in the internal store, keyed the same way as the owner record
 * above. @name is the full xattr name (e.g. "security.capability");
 * callers should route here only when dn_policy_is_fake_xattr(name) is
 * true. */
int dn_policy_is_fake_xattr(const char *name);
int dn_policy_fake_setxattr(const char *host_path, dev_t dev, ino_t ino, const char *name, const void *value, size_t size);
int dn_policy_fake_getxattr(const char *host_path, dev_t dev, ino_t ino, const char *name, void *value_out, size_t cap);
int dn_policy_fake_removexattr(const char *host_path, dev_t dev, ino_t ino, const char *name);

/* ---- Hardlinks ------------------------------------------------------ */

/* link2symlink (runtime.md, "Hardlinks" -- a clean-room reimplementation
 * of PRoot's extension of the same name; see runtime.md's resolved
 * open item on why copying the idea needed no license decision).
 *
 * dn_policy_link() implements link(@existing_host_path,
 * @new_host_path): moves @existing_host_path's content into a hidden
 * file on first use, making both names symlinks to it with a shared
 * link count. @existing_host_path must be an ordinary regular file the
 * first time this is called on it; a later call (a third name for the
 * same content) recognizes it's already a managed symlink and just adds
 * another one. dn_policy_fake_stat() (above) already rewrites
 * st_nlink/st_mode/st_dev/st_ino for these names so they report as an
 * ordinary multiply-linked regular file, never a symlink, under either
 * stat() or lstat() -- callers don't need a separate function for that,
 * only to route fake_stat() every stat-family result through it as
 * already documented.
 *
 * dn_policy_unlink() decrements the link count for whatever
 * @host_path's *current* symlink target is, and removes the hidden file
 * once it reaches zero. ORDER MATTERS: call this *before* the real
 * unlink(@host_path) runs -- once that real unlink happens there is no
 * symlink left here to read the target back out of. Not an error if
 * @host_path isn't a managed name at all (an ordinary file or symlink);
 * that case is simply a no-op, since this is only bookkeeping and the
 * real unlink() is the caller's to make regardless. */
int dn_policy_link(const char *existing_host_path, const char *new_host_path);
int dn_policy_unlink(const char *host_path);

#ifdef __cplusplus
}
#endif

#endif /* DN_POLICY_H */
