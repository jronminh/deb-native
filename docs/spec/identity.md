# Identity: one real uid, virtual principals

Status: **proposal** (2026-10-17).  How identity works today is in
[`../architecture.md`](../architecture.md), "Identity: fake root"; the syscall
map is [`../reference/syscall-classes.md`](../reference/syscall-classes.md).

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
| **real principal** | the app's uid/gid | itself (user mode) | the app sandbox |
| **virtual principals** | the app's uid/gid | `/etc/passwd`, `/etc/group`, `user.dn.owner`, a persona | **none** |

The runtime must say this plainly.  A user created inside a prefix is not an
Android user: it cannot be logged into outside the prefix, it has no separate
sandbox, and it does not restrict what the app can do.  Virtual identity is for
**compatibility** -- file ownership as a Debian program expects it, daemons that
drop to a service user, package maintainer scripts -- never for security.

## The personas

A process has one **persona**, chosen at boot or at exec, inherited by fork and
exec:

- **user persona** -- the real principal.  `getuid()` is the app's uid;
  `stat` is the kernel's (plus a stored owner, below); `setuid` to another id
  is `EPERM`; `chown` is `EPERM`; `access` is the kernel's.  This is an
  ordinary non-root Linux user.
- **root persona** -- the virtual principal `root` (uid 0).  `getuid()` is 0,
  `set*id` is remembered per process, `chown` is stored, `access` is emulated
  as root, `/proc/<pid>/status` shows root.  This is the compatibility view
  `apt`/`dpkg`/`useradd` need; it is **not** privilege: the kernel still sees
  the app.

A **service-user persona** (any other virtual uid, e.g. `www-data`) is like
the user persona for authority -- no root view, no capabilities -- but its
identity and files are virtual: `getuid()` and `stat` say that uid, `setuid`
to it is allowed from the root persona, and its files carry an owner store
entry.  It is a normal (non-root) virtual principal.

Personas are per process, not per prefix.  Fork and exec inherit; a shell can
start a root command and a user command side by side.  `ps` inside the tree
shows each process's persona identity.

## Tree-root is not real root

Because the app owns every file in the prefix, the app is the real *owner* of
the tree.  That makes the user persona's file access honest (owner bits apply),
but it also means:

- the root persona's `chown` and ownership are **stored**, not kernel-enforced;
- a file stored as `root:root` with mode 0644 is still writable by the app, so
  a virtual user can be more permissive than its Debian counterpart;
- `setuid`/`setgid` bits do not grant kernel privilege; a setuid-root binary
  runs with the app's real credentials;
- capabilities shown as root's are a view; the kernel's are the app's;
- DAC override (`mode 000`, a file owned by another host uid, a bind from
  `/sdcard`) is not available to either persona; the kernel answers `EACCES`;
- ownership-based security in programs (`sshd` StrictModes, `su`, `login`) can
  see the virtual owner and reach a different conclusion than the kernel.

These are accepted gaps, not bugs to patch with more fakery.  The rule is the
same as everywhere: **map what can be mapped, emulate what is harmless, refuse
what would be a lie about authority**.

## Syscall view by persona

| call | user / service persona | root persona |
| --- | --- | --- |
| `get*id`, `getgroups` | real app id | 0 / remembered state |
| `set*id` | kernel `EPERM` (or `EPERM` answered) | remembered per process |
| `stat` owner | real id, or the stored owner | stored owner, else 0 |
| `chown` | `EPERM` | `user.dn.owner` xattr |
| `access`, `euidaccess` | kernel | emulated as root |
| `/proc/<pid>/status` | real Uid/Gid | 0, root's `Cap*` view |
| `setuid` binary exec | persona from the setuid bit (below) | same |

## Setuid binaries as the elevation gate

A virtual identity needs a **transition**: user -> root, root -> service user,
and so on.  The closest thing to the kernel's mechanism is the setuid bit,
which the exec gate can read (the owner comes from `user.dn.owner`):

- `sudo` (mode 4755, stored owner root): a user-persona process that execs it
  gets the root persona, the way the kernel would give it euid 0; `sudo`'s PAM
  and `/etc/sudoers` then run as they do on Debian, and its `setuid(target)`
  switches the persona down to the target user.
- `su`, `passwd`, `login` can follow the same rule.
- A user-persona process **cannot** become root by calling `setuid(0)`; only
  the setuid-bit gate (and the runtime's own boot/session switch) changes
  personas upward.

This is **compatibility theatre, not a security boundary**: the app user owns
the tree and can replace `sudo`, clear its setuid bit, read `/etc/shadow`, or
call the runtime's switch.  It exists so that the real Debian binaries behave
as their documentation says; it must never be presented as isolation.

## Files, groups, and the owner store

- One xattr, `user.dn.owner`, holds `UID:GID` for a file whose stored owner is
  not the real id.  A chown to 0:0 removes it.  The copy `linkat` makes carries
  it.
- `useradd`/`groupadd` write `/etc/passwd` and `/etc/group` entries; `userdel
  -r` works because the owner store makes the home directory look owned by the
  user it removes.  These are **virtual users**: the runtime should say so,
  and a login for one only exists inside the prefix (through a persona switch),
  never as an Android user.
- Supplementary groups are virtual too: `getgroups`/`initgroups` can be
  answered, the file's gid can be stored, but the kernel's access checks use
  the app's gid, so group permissions are not enforced.
- The fast path pays for the owner store on every `stat` that reads it.  Two
  optimisations are planned and orthogonal: an owner cache keyed by
  `(dev, ino, ctime)` (chown changes ctime, so the cache cannot go stale), and
  a shared "owner store has ever been used" flag so a prefix that never chowns
  reads no xattr at all.

## Current state (2026-10-17)

- Every process boots as the **root persona**; there is no user persona or
  persona switch yet.
- `chown` stores `user.dn.owner` for any uid:gid; the tracer and the shim read
  it back in `stat`.  A hard-link copy carries it.
- `set*id` remembers any uid in the root persona; `get*id` answers it; the shim
  answers the `get*id`/`getres*id` family in process.
- `useradd`/`userdel -r` work against the virtual identity (checked on `p-dev`).
- Root's capabilities are a `/proc` view; `capget`/`PR_CAPBSET_READ` are not
  answered.
- There is no setuid-bit persona gate, no `sudo`/`su` wrapper, and no real
  multi-user.

## Plan

1. **Persona state** in the tracer and the shim: `user`/`root` (and later any
   virtual uid), chosen at boot (`boot/env`, or a `--persona` option) and
   inherited through fork/exec; the exec gate propagates it through a clean
   environment.
2. **User persona behaviour**: real `get*id`, `set*id` -> `EPERM`, real
   `access`, `chown` -> `EPERM`, `stat` without the real-id-to-0 map; the
   stored owner is still read when present.
3. **Virtual users and groups**: passwd/group entries written at boot for the
   real uid (the boot script knows it, the tree does not); setuid to any
   virtual uid from the root persona; `getgroups`/`initgroups`; useradd UX
   says "virtual user".
4. **Setuid-bit gate** (optional, after 1-3): the exec gate reads the setuid
   bit and the stored owner and starts the child in the matching persona, so
   the real `sudo`/`su`/`passwd` binaries can run.
5. **Owner cache and the store-used flag** (independent; do early): remove the
   per-stat xattr cost.
6. **Tests**: the matrix in `../../tests/bench/` -- a user-persona prefix with
   `git`, `python`, `tar`, `make`, `ssh-keygen` (expected to work); `apt-get`,
   `dpkg -i`, `useradd`, `passwd` (expected to need the root persona); a
   setuid-bit `sudo`; a service user that a package creates; `stat` on stored
   owners; the authority mismatches (0644/0600/000, a bind from the host);
   the fast path before and after the first chown.

## Open questions

- **Default persona**: root (Debian's habit, `apt` just works) or user (the
  honest one, with `sudo` for package management)?  Decide with the test
  matrix, not by taste.
- **Real sudo or a wrapper**: the setuid gate lets the real binary run, but its
  PAM sessions, tty handling and `setgroups` widen the emulation surface.  A
  small wrapper is less faithful and less risky.  Try the real one first.
- **Capabilities**: keep the root view in `status` but refuse `capget`, or
  answer a Debian default?  Answering a default is more complete and also more
  of a lie; `refuse` is the current rule.
- **Binds from the host**: a user persona on a bind sees the host's owner and
  permissions; that is the kernel's truth and should stay so, but it is the
  first place a "virtual user" will look wrong.
