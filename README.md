# termux-deb-bridge

Install real Debian `.deb` (glibc, `linux-arm64`) packages on Termux/Android
— without patching every binary by hand, and without the parts of the
`sudo-less` approach that Android's kernel/SELinux won't allow.

Status: **R&D, not functional yet.** No install path works end to end. This
repo currently holds design notes and a comparison against prior art, not
working code.

## Why this exists

Termux ships its own package repo, rebuilt against Bionic (musl-like NDK
libc). A huge amount of the Debian archive never gets rebuilt for Termux.
Meanwhile a real Debian `.deb` for `arm64` is a normal glibc ELF binary —
Termux can already load glibc binaries one at a time via `glibc-runner`
(patch the ELF interpreter, run against a glibc side-install), but doing
that per-binary, by hand, does not scale to "apt-get install anything".

[`sudo-less`](https://github.com/jronminh/sudo-less) solves a related but
different problem: real `apt`+`dpkg` installing Debian packages into
`~/.local` **on a real Debian host**, no root, using a private mount
namespace that overlays the prefix onto `/usr /etc /var /opt` ("the view")
so a package's hardcoded absolute paths still resolve. Its docs are the
valuable part — see [`docs/prior-art.md`](docs/prior-art.md) for the full
breakdown of what carries over to Termux and what doesn't.

**The short version: about 40% of sudo-less's approach is reusable as-is.**
The other 60% — the mount-namespace + overlayfs "view", and the
`systemd --user` service layer — assumes machinery Android typically
denies to an app-sandboxed process (`unshare(CLONE_NEWUSER)` is blocked by
SELinux on most stock ROMs even when the kernel supports it) or doesn't
have at all (no systemd on Termux).

## Approach

| sudo-less piece | on Termux |
|---|---|
| apt/dpkg forked to run root-less in a prefix, patches for no-superuser-check/chown/ldconfig-check | **not needed as a fork** — this *is* what Termux's own apt/dpkg patches already do, they're just built for Bionic. Reusable directly. |
| two-layer package db (host's `dpkg` status as read-only lower layer) | reusable idea: treat Termux's existing package set as the "already installed" layer, only fetch/install glibc leaf packages |
| `prefix-wrap` heuristics (does a binary need path-resolution help: absolute symlink out of prefix, missing interpreter, `ldd`-missing lib, hardcoded `/usr|/etc|/opt` path) | reusable as detection logic, independent of how the fix is applied |
| the "view": private mount ns + unprivileged overlayfs, live-patching path resolution at run time | **blocked on Android** (SELinux denies `unshare(CLONE_NEWUSER)` to Termux's app domain on most devices) — being replaced, see [Direction 2](docs/design-static-wrappers.md) |
| services via `systemd --user`, translated unit by unit | **doesn't exist on Termux** — being researched against `termux-services` (runit), see [Direction 3](docs/services-research.md) |

Install path decision (no fork/patch of apt/dpkg needed — see
[`docs/design-install-path.md`](docs/design-install-path.md)): Termux's own
apt/dpkg already carry the non-root patches sudo-less had to add for a real
Debian host, so this project reuses them as-is, relocated to a separate
prefix via dpkg's own `--instdir`/`--force-script-chrootless` flags and a
custom `apt.conf` — not a source fork.

This repo is pursuing two directions in place of the blocked 60%:

1. **[Direction 2 — static per-binary wrappers](docs/design-static-wrappers.md).**
   Instead of a live mount-namespace overlay, generate a fixed wrapper script
   per binary at install time (same detection heuristics as `prefix-wrap`),
   pointing it at prefix paths directly (`--config`, env vars, or a
   glibc-runner-patched ELF interpreter) instead of making `/etc/foo.conf`
   resolve live. Less general, no namespace required, works everywhere
   Termux does.
2. **[Direction 3 — services on `termux-services` (research)](docs/services-research.md).**
   No `systemd --user` on Termux; `termux-services` (runit-based) is the
   native equivalent. Researching whether a package's systemd unit can be
   translated to a runit service script the way sudo-less translates it to
   a user unit.

## Non-goals

- Not a container, not a second distribution (that's `proot-distro`).
- Not trying to reproduce sudo-less's sandbox (`prefix-sandbox`,
  seccomp filters tied to the namespace) — that whole layer depends on the
  view, which isn't available here.
- Not for packages that need root at install or run time (system daemons
  with real system users, `setuid` binaries) — same caveat sudo-less states
  for its own prefix installs.

## Prior art / credit

- [`sudo-less`](https://github.com/jronminh/sudo-less) — the apt/dpkg
  prefix-install approach and its docs are the starting point for this
  repo's design. See [`docs/prior-art.md`](docs/prior-art.md) for the
  detailed carry-over analysis.
- Termux's own `apt`/`dpkg` patches (`termux/termux-packages`) — the
  original source sudo-less itself forked from; ends up being the piece
  this repo needs least modified, since it's already built for this exact
  environment.
