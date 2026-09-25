# Prior art: what carries over from sudo-less

Source: [`jronminh/sudo-less`](https://github.com/jronminh/sudo-less),
docs read 2026-09-25 (`README.md`, `docs/porting.md`,
`docs/apt-dpkg-port.md`, `docs/view.md`).

## What sudo-less actually is

Real `apt`+`dpkg`, forked from **Termux's own patches** to those two
projects (`termux/termux-packages`), retargeted from Android/Bionic back
onto a regular rootless Debian host, installing into `~/.local`. Three
Debian-native mechanisms do the work: unprivileged user+mount namespaces,
unprivileged overlayfs (kernel ≥5.11), and `systemd --user`.

The irony worth noting: sudo-less took Termux's patches *out* of the
Android context to make them work on desktop Debian. This repo needs to
partly reverse that back onto Android — but Termux's own apt/dpkg already
covers the parts sudo-less had to rebuild, so that part doesn't need
touching at all.

## Reusable as-is (~40%)

1. **apt/dpkg running root-less in a prefix.** Termux's apt/dpkg already do
   this natively (`$PREFIX=/data/data/com.termux/files/usr`), no fork
   needed. sudo-less's patches (`0001-no-superuser-check`, `0002-no-chown`,
   `0003-no-ldconfig-check`) are literally Termux's own Android-guarded
   changes made unconditional — Termux already has the unconditional
   Android version.

2. **Two-layer package database.** sudo-less seeds the prefix's dpkg status
   from the host's `/var/lib/dpkg/status` so system libraries count as
   "already installed" and apt doesn't try to pull the whole `libc6` chain.
   Direct analogue here: treat Termux's own installed package set as the
   read-only lower layer, so a glibc `.deb` install only pulls the leaf
   packages actually missing (glibc runtime itself, plus whatever the
   package needs that Termux doesn't ship in Bionic form).

3. **`prefix-wrap`'s detection heuristics** (`docs/view.md`, "How programs
   get there" table) — a binary needs help resolving its own paths when
   (first match): it's a symlink leaving the prefix, its interpreter
   shebang isn't on the host, its interpreter only searches compiled-in
   module paths (Python/Perl/Ruby/Node/...), `ldd` can't find one of its
   libraries, or it names a file/dir of its own under `/usr`, `/etc`,
   `/opt` that the prefix has. This detection logic is independent of *how*
   the fix gets applied (mount namespace vs. static wrapper) — reusable
   directly for Direction 2.

## Blocked on Android (~60%)

1. **The "view"** (`docs/view.md`): a private mount namespace overlaying
   the prefix onto `/usr /etc /var /opt`, live, so absolute paths compiled
   into a binary resolve against the prefix. Needs `unshare(CLONE_NEWUSER)`
   + unprivileged overlayfs in that namespace. **Confirmed blocked on this
   device, with the actual syscall error** (see
   `design-manual-overlay.md`): `unshare(CLONE_NEWUSER)` fails with
   `EINVAL` (not `EPERM`) — the kernel itself doesn't support unprivileged
   user namespaces at all here, not merely an SELinux policy denial. Plain
   `unshare(CLONE_NEWNS)` alone fails with `EPERM` as expected (needs
   `CAP_SYS_ADMIN`). FUSE is also closed (`/dev/fuse`: permission denied,
   no `fusermount`). Replaced by a userspace `LD_PRELOAD` path-redirect
   shim instead — see `design-manual-overlay.md`, verified working against
   a real package (`figlet`).

2. **`prefix-sandbox`'s seccomp/namespace-based isolation** — depends
   entirely on the view above, so it goes with it.

3. **Services via `systemd --user`** — Termux has no systemd at all (no
   init, no cgroups in the relevant sense, no user manager). Native
   equivalent is `termux-services` (runit). See
   [`services-research.md`](services-research.md).

## Formerly-open question, now answered

~~Whether `unshare(CLONE_NEWNS)` alone... is available to Termux without
root.~~ Answered: no — `EPERM`, confirmed by direct test and strace. See
`design-manual-overlay.md` for the full data and the replacement mechanism
(userspace `LD_PRELOAD` path redirection, no mount involved at all).
