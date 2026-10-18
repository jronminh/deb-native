# Identity: one real uid, virtual principals

Status: **implemented** (2026-10-18).  How the paths and `/proc` view work is
in [`../architecture.md`](../architecture.md), "Identity"; the syscall map is
[`../reference/syscall-classes.md`](../reference/syscall-classes.md).

A prefix is one Android app with **one real kernel identity**: the app's uid and
gid.  Every other identity a Debian program can name -- `root`, `www-data`,
`postgres`, `nobody`, a user someone creates -- is a **virtual principal**: a
name and a number the runtime keeps, never a second kernel uid.  This is a
property of the platform, not a limitation the runtime can lift: an app cannot
`chown` or `setuid` outside its own uid, and a user namespace would still map a
single host id (no subordinate ranges, and Android's SELinux and UID space do
not give it more).

So there are two layers:

| layer | in the kernel | in the tree | isolation |
| --- | --- | --- | --- |
| **real principal** | the app's uid/gid | itself | the app sandbox |
| **virtual principals** | the app's uid/gid | `/etc/passwd`, `/etc/group`, `user.dn.owner`, the fake ids | **none** |

The runtime must say this plainly.  A user created inside a prefix is not an
Android user: it cannot be logged into outside the prefix, it has no separate
sandbox, and it does not restrict what the app can do.  Virtual identity is for
**compatibility** -- file ownership as a Debian program expects it, daemons that
drop to a service user, package maintainer scripts -- never for security.

## No mode: the euid is the classifier

There is no boot "persona" and no switch.  Every syscall is classified by the
calling process's **current effective uid** (the `ids` state the tracer keeps
per process); fork and exec inherit it, and an identity change is the transition.

| case | euid | what the program gets |
| --- | --- | --- |
| **root** | `0` | fake root: `set*id` remembered, `chown` stored, root's `/proc/status` view, groups `{0}` |
| **service user** | a virtual uid, not the app's | its fake uid; no root: `set*id`/`chown` `EPERM`, no capabilities; its virtual group list |
| **real user** | the app's uid | the kernel's: real `get*id`, the app's groups at boot, `set*id`/`chown` `EPERM`, real `access` and `/proc/status` |

`get*id` always answers the state; in the real user case the state *is* the app
uid, so the answer is the kernel's.  `stat` ownership is a property of the
**file**: a stored `user.dn.owner` wins in both worlds, and a file with no store
reads `0:0` in **both** (the implicit root -- a legacy file, or a symlink, which
takes no user xattr).  A `chown` is stored for any uid:gid, 0:0 included, so an
explicit owner is the same in every case.

## The boot seed

The boot chooses only the **identity the first process starts with**:

- the default is the app's **real uid** -- an ordinary user session;
- `DN_INITIAL=root boot/boot.sh` starts a root session (the package-management
  compatibility view, and what the test suite uses).

That is a seed, not a mode: after the first process, everything follows from
the euid.  `init.sh` writes `/etc/passwd`, `group` and `shadow` lines for the
app uid from `DN_REAL_UID`/`DN_REAL_GID`, and sets `HOME` to `/home/user` for
the real uid or `/root` for seed root.

## Transitions

- **Root -> anything**: `setuid`/`setresuid` are emulated in the root case; the
  new euid classifies the process from the next syscall.  `su user` reduces to
  `setuid(real_uid)`.
- **Non-root -> root**: only through a **setuid-bit exec** (below) or the boot
  seed; `setuid(0)` from a non-root euid answers `EPERM`.
- **Root -> service user**: the same `setuid`/`setresuid`, then the process is
  a service user; e.g. a daemon dropping to `www-data`.

## The setuid bit is the gate

The exec gate reads the final program's mode and stored owner (from
`user.dn.owner`; **without a store it is root's**, as a package's file is):

- `4755`, stored owner 0: the child gets **euid = suid = 0** (the real uid
  stays what it was, as a real setuid binary does).
- `4755`, stored owner the app uid: the child gets euid = suid = the app uid (a
  drop, as a setuid binary owned by the user would do).
- anything else: the ids are inherited.

The gate hands the new shim its six ids in `DN_IDS`
(`ruid:euid:suid:rgid:egid:sgid`, a hint, so `get*id` and a stat's
classification need no tracer stop); a `set*id` in the process invalidates it
and the shim asks the tracer again.  `getgroups` answers the per-process
virtual list: root starts with `{0}`, the real user with the app's groups,
a service user with its own; `setgroups`/`initgroups` (root only) replace it.

Real `sudo` behaviour needs one more step, and a sudo-like helper does it: after
the gate gives it euid 0, it calls `setgid(0)` and `setuid(0)` (emulated, since
it is root) so the command sees **uid 0 too**.  A shell started with euid 0 but
real uid non-zero drops back to the real uid, as a setuid program's child shell
should; a shell whose uid is 0 does not.  This is why a helper that only execs
the command is not enough.

This remains **compatibility theatre, not a security boundary**: the app owns
the tree and can replace the helper, clear its setuid bit, or read
`/etc/shadow`.  It exists so the real Debian binaries and their documentation
behave; it must never be presented as isolation.

### What a root program expects beyond the ids

Real programs check more than `geteuid()`; the runtime answers the checks, not
the privilege:

- **DAC override.**  The fake root cannot write a file whose owner bits lack
  write (a 0440 `.dpkg-new` is the usual case).  A write open by euid 0 adds the
  owner write bit for the open and restores the mode at the exit stop; the fd
  keeps the access it was opened with.  A directory open adds write+execute.
- **`NoNewPrivs`.**  Android sets it; sudo refuses to run with it.  It reads 0
  in `/proc/<pid>/status` and `prctl(PR_GET_NO_NEW_PRIVS)` answers 0.
- **`PR_SET_DUMPABLE`.**  sudo clears it, which would stop the tracer reading
  its memory (`process_vm_readv` and `/proc/<pid>/mem`); the prctl is answered
  without changing the flag, and `mon_read`/`mon_write` fall back to
  `PTRACE_PEEKDATA`/`POKEDATA` when the vm calls are refused.
- **An unmarked setuid file** (no `user.dn.owner`) is taken as a package's,
  owned by root, so the gate gives euid 0; a setuid file with a stored owner
  elevates to that owner.
- **The audit hook** exports glibc's whole audit symbol set and clears the lane
  cookie after each loader call; without that, a setuid program's `RUNPATH`
  libraries (sudo's `libsudo_util`) were opened as host paths.

## Tree-root is not real root

Because the app owns every file in the prefix, the app is the real *owner* of
the tree.  That makes the real user's file access honest (owner bits apply),
but it also means:

- a virtual `chown`/ownership is **stored**, not kernel-enforced;
- a file stored as `root:root` with mode 0644 is still writable by the app, so
  a virtual user can be more permissive than its Debian counterpart;
- `setuid`/`setgid` bits do not grant kernel privilege; the gate only changes
  the ids state;
- capabilities shown as root's are a view; the kernel's are the app's;
- DAC override (`mode 000`, a file owned by another host uid, a bind from
  `/sdcard`) is not available in any case; the kernel answers `EACCES`;
- ownership-based security in programs (`sshd` StrictModes, `su`, `login`) can
  see the virtual owner and reach a different conclusion than the kernel.

These are accepted gaps, not bugs to patch with more fakery.  The rule is the
same as everywhere: **map what can be mapped, emulate what is harmless, refuse
what would be a lie about authority**.

## Files, groups, and the owner store

- One xattr, `user.dn.owner`, holds `UID:GID` for every file that was chowned
  (0:0 included) or created.  A create is marked with the creator's `euid:egid`
  by the tracer (`openat` with `O_CREAT`, `mkdirat`, `mknodat`) or by the shim
  (`mkdirat`/`mknodat` on the lane), so a file created by root reads as root
  for the real user, and one created by the real user reads as that uid in the
  fake world.  A later `chown` overwrites it.  A file with no store is the
  **implicit root's in both worlds**: a file that predates marking, and a
  symlink (which takes no user xattr), read `0:0` to the fake root and to the
  real user alike, so the two worlds cannot disagree about it.  (`chown -h` on a
  symlink is answered but cannot be stored; a symlink's owner is cosmetic, its
  target's owner is what access checks.)  The copy `linkat` makes carries the
  source's xattr.
- `useradd`/`groupadd` write `/etc/passwd` and `/etc/group` entries; `userdel
  -r` works because the owner store makes the home directory look owned by the
  user it removes.  These are **virtual users**: a login for one exists only
  inside the prefix (through an identity change), never as an Android user.
- Supplementary groups are a per-process **virtual list**: root starts with
  `{0}`, the real user with the app's groups (read at boot), and
  `setgroups`/`initgroups` replace it (root only).  The list is sparse and has
  no fixed cap (the kernel's `NGROUPS_MAX` is the limit): a process that never
  calls `setgroups` shares the boot seed.  A service user reads that list.
  `access()`/`faccessat` answer from this identity -- root passes (with the
  execute-bit rule), anyone else is checked by the stored owner and the group
  list -- in the shim when the owner or group class decides and in the tracer
  otherwise.  A real `open` is still the kernel's check against the app's gid;
  group permissions are the **view** the program asks for, not a kernel
  boundary (the app owns the tree).
- The fast path reads the owner store through a cache keyed by
  `(dev, ino, ctime)` in both the shim and the tracer: chown changes ctime, so
  an entry cannot go stale; a collision costs a getxattr.  Measured on p-dev,
  5000 `stat`s of one file: 34 ms before, 22 ms after.  A shared "owner store
  has ever been used" flag, to skip even the first xattr per file on a tree
  that never chowns, is still open: a copied prefix can carry stored owners
  with no flag, so it needs a correctness story.

## Current state (2026-10-18)

- Automatic classification by euid is implemented in the tracer (fake ids per
  process) and the shim (`DN_IDS` hint, else the traced ids).
- Default boot is the real app uid; `DN_INITIAL=root` seeds a root session.
  `init.sh` writes the real uid's passwd/group/shadow lines and `/home/user`.
- In the real user case: real `id`, `get*id`, groups and `/proc/status`;
  `set*id`/`chown` `EPERM`; `git`, `python`, `tar`, `git init` and `apt-get
  update` were checked.  `useradd` and `dpkg -i` need root, as on Debian.
- The setuid gate is implemented for euid (owner 0 -> 0; owner real -> real).
  A small `dn-sudo` helper (4755, stored owner 0, calls `setgid(0)`/`setuid(0)`)
  gives a root shell: `uid=0`, `chown` works, `apt-get install` and `useradd`
  work, and the parent stays the real user.  A service user (euid 33) reads
  `stat` as root-owned, cannot chown, and has no capabilities.
- A create is marked with the creator's ids: `openat`+`O_CREAT`, `mkdirat` and
  `mknodat` in the tracer, `mkdirat`/`mknodat` in the shim's lane; the marking
  costs an exit stop per **new** file (Lua: 79 exit stops, wall unchanged).
- Groups: `getgroups` returns the process's virtual list; `setgroups`/
  `initgroups` (root only) replace it.  Checked with a system user
  (`adduser --system`, uid 100, gid 102): after `initgroups('svctest', 102)`
  and `setuid`, `getgroups` is `[102]` and the created file is `100:102` in
  both worlds.  The list has no fixed cap (40 entries were tested).
- `chown` is stored for any uid:gid; the cache keeps `stat` cheap; a hard-link
  copy carries the owner.  Root's capabilities are a `/proc` view;
  `capget`/`PR_CAPBSET_READ` are not answered.
- Legacy files and symlinks read `0:0` in both worlds (no store), so the
  regression set's digest is now the same for the root seed and the real user;
  a new file or `chown` carries the creator's stored owner.
- `access()`/`faccessat` answer the virtual view: a 0440 file is writable to the
  fake root (its DAC override), `/etc/shadow` is not readable to the real user,
  and a service user is judged by its stored owner and group list (a
  root:102 0640 file is readable to a member of 102).  The shim answers the
  clear cases without a stop; the tracer answers the rest (and static
  programs).
- `useradd`/`adduser` are wrapped in `/usr/local/sbin` (now on `PATH`): they
  print that users and groups here are virtual (one kernel uid, no Android
  account) and then run the real tool.
- **Real `sudo`** is installed in the test prefix with
  `/etc/sudoers.d/user` (`user ALL=(ALL) NOPASSWD: ALL`); `sudo -n id` prints
  `uid=0`, `sudo -n apt-get update`, `sudo -n useradd`/`userdel` work, and the
  parent shell stays the real user.  It needed those answers above, not a
  wrapper.
- **Real `su` and `passwd`** run through PAM: `su - root` from the real user
  authenticates from `/etc/shadow` and gives uid 0, a `/root` cwd and
  `HOME=USER=LOGNAME=root`; a wrong password is refused; `su` from the root
  seed to the real user, a service uid and root all work (sets `initgroups`,
  the target's home and shell); `passwd` changes a password both as the fake
  root and as a user proving the old one, and a wrong old password leaves the
  hash untouched.  The regression set runs `su` from the root seed.
- Check suite, coverage and fingerprint are clean with the root seed; the Lua
  build is 864 entry + 79 exit stops (root) and 872 + 79 (real user), wall
  ~12-14 s (the prctl row is the +16 over the row set before the sudo work).

## Plan / open questions

1. **Virtual service users**: the group list, file gid and `access()` are in;
   still open: the kernel's own `open` checks the app's gid, so a service user
   can open more than `access()` says.  The `useradd` wrapper says "virtual
   user".
2. **Real `sudo`, `su` and `passwd`**: tested (sudo NOPASSWD with apt and
   useradd; su login and password auth; passwd as root and as the user).  The
   wrapper remains the fallback.
3. **Legacy and symlink creations**: closed -- a file with no store reads
   `0:0` in both worlds.  A symlink's owner is not stored (no user xattr); it is
   cosmetic.
4. **Capabilities**: keep the root view in `status` but refuse `capget`, or
   answer a Debian default?  `refuse` is the current rule.
5. **Binds from the host**: a real user on a bind sees the host's owner and
   permissions (the kernel's truth); the fake world's mapping of the real uid
   to 0 may disagree with the host's ids there.
