# Identity: one real uid, virtual principals

Status: **implemented** (2026-10-17).  How the paths and `/proc` view work is
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
| **root** | `0` | fake root: `set*id` remembered, `chown` stored, root's `/proc/status` view, `getgroups` one group 0 |
| **service user** | a virtual uid, not the app's | its fake uid; no root: `set*id`/`chown` `EPERM`, no capabilities |
| **real user** | the app's uid | the kernel's: real `get*id`, real groups, `set*id`/`chown` `EPERM`, real `access` and `/proc/status` |

`get*id` always answers the state; in the real user case the state *is* the app
uid, so the answer is the kernel's.  `stat` ownership is a property of the
**file**, decoded for the process's world: a process whose euid is not the app's
(the fake world -- root or a service user) reads the real ids as 0, while the
real user reads them as they are; a stored `user.dn.owner` wins in both.  A
`chown` is stored for any uid:gid, 0:0 included, so an explicit owner is the
same in every case.

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
`user.dn.owner`; without one, the real uid is the owner):

- `4755`, owner 0: the child gets **euid = suid = 0** (the real uid stays
  what it was, as a real setuid binary does).
- `4755`, owner the app uid: the child gets euid = suid = the app uid (a drop,
  as a setuid binary owned by the user would do).
- anything else: the ids are inherited.

The gate hands the new shim its six ids in `DN_IDS`
(`ruid:euid:suid:rgid:egid:sgid`, a hint, so `get*id` and a stat's
classification need no tracer stop); a `set*id` in the process invalidates it
and the shim asks the tracer again.  `getgroups` is interposed too: root's fake
view is one group 0, everyone else gets the kernel's through the lane.

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

- One xattr, `user.dn.owner`, holds `UID:GID` for every file that was chowned,
  0:0 included.  The only implicit owner is the real uid: the fake world maps
  it to 0, the real user leaves it.  A file created without a chown can
  therefore read as 0 in the fake world and as the real uid in the real user
  case (and the reverse for a user-created file); marking creations is open.
  The copy `linkat` makes carries the xattr.
- `useradd`/`groupadd` write `/etc/passwd` and `/etc/group` entries; `userdel
  -r` works because the owner store makes the home directory look owned by the
  user it removes.  These are **virtual users**: a login for one exists only
  inside the prefix (through an identity change), never as an Android user.
- Supplementary groups are virtual too: `getgroups`/`initgroups` can be
  answered in the fake world, the file's gid can be stored, but the kernel's
  access checks use the app's gid, so group permissions are not enforced.  The
  real user gets the kernel's groups.
- The fast path reads the owner store through a cache keyed by
  `(dev, ino, ctime)` in both the shim and the tracer: chown changes ctime, so
  an entry cannot go stale; a collision costs a getxattr.  Measured on p-dev,
  5000 `stat`s of one file: 34 ms before, 22 ms after.  A shared "owner store
  has ever been used" flag, to skip even the first xattr per file on a tree
  that never chowns, is still open: a copied prefix can carry stored owners
  with no flag, so it needs a correctness story.

## Current state (2026-10-17)

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
- `chown` is stored for any uid:gid; the cache keeps `stat` cheap; a hard-link
  copy carries the owner.  Root's capabilities are a `/proc` view;
  `capget`/`PR_CAPBSET_READ` are not answered.
- Check suite, coverage and fingerprint are clean with the root seed; the Lua
  build is ~11.9 s / 846 stops (root) and ~11.7 s / 854 (real user).

## Plan / open questions

1. **Creation marking**: mark a file at creation with its creator's identity
   (root 0:0, service uid, or the real uid) so ownership no longer depends on
   the reader's euid; the reverse asymmetry (a file created without a chown)
   goes away.  Needs a pre-stat and an exit stop on create, or a shim path.
2. **Virtual service users and groups**: `getgroups`/`initgroups` for a service
   user (today it falls back to the kernel's groups), file gid, useradd UX that
   says "virtual user".
3. **Real `sudo` and `su`**: the gate lets the real binary start with euid 0;
   its PAM/sudoers/setgroups surface remains to be tested.  A wrapper is the
   fallback.
4. **Capabilities**: keep the root view in `status` but refuse `capget`, or
   answer a Debian default?  `refuse` is the current rule.
5. **Binds from the host**: a real user on a bind sees the host's owner and
   permissions (the kernel's truth); the fake world's mapping of the real uid
   to 0 may disagree with the host's ids there.
