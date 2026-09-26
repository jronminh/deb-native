# AGENTS.md — deb-native

## What this repo is

`deb-native` installs and runs real Debian **arm64** (`.deb`) packages inside
**Termux** on unrooted Android — **no root, no `proot`, no `chroot`, no
kernel namespaces**. It fakes the Debian *layout* (`/usr /etc /var /opt`) with
an `LD_PRELOAD` libc shim that rewrites those paths to `$INSTDIR`, while
everything else stays real: real `aarch64`, real glibc (Termux's
`$PREFIX/glibc` side-install), Termux's own `apt`/`dpkg`, real ELF loading.

The goal is narrow: a package reaches `dpkg` status **`ii`**, and its program
runs **by name**, unprivileged. Not a faithful Debian, not an emulator and not
isolation — "install and run, not emulate".

Two mechanisms:

- **Maintainer scripts** — plain-text path rewriting before `dpkg` runs them
  (`scripts/patch-maintainer-scripts.sh`).
- **Installed binaries** — `native/path-redirect.c`, an `LD_PRELOAD` glibc
  shim that interposes path-taking libc functions and rewrites
  `/usr /etc /var /opt` → `$INSTDIR`. Its `execve` dispatch keeps the preload
  for a glibc child and strips it for a Bionic one.

Relationship to **sudo-less** (`~/sudo-less`): the same goal on a real Debian
host, using a kernel mount-namespace + unprivileged overlayfs "view". Here the
view is impossible (user namespaces are off kernel-wide), so the shim replaces
only the view's *path-resolution* job.

## Layout

- `native/path-redirect.c` — the libc interposition shim; build with
  `scripts/build-path-redirect.sh`.
- `native/dn-launch.c`, `native/dn-run.c` — Bionic launcher / per-binary
  shim-vs-proot dispatch.
- `scripts/` — base bootstrap, native-dependency seeding, apt/dpkg wiring,
  path/maintainer-script patching, launchers, surveys, and
  `scan-libc-symbols.sh`.
- `tests/shim-libc/` — on-device smoke test for the shim.
- `docs/` — `design.md` (the design), `findings.md` (engineering log),
  `vs-sudo-less.md` (side-by-side diff).
- `TODO.md` — roadmap, ordered like sudo-less's.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the Termux
  user with the glibc toolchain). It pushes to GitHub `main`.
- A local working copy may be **stale**. Edit locally, `scp` files to `fe2`,
  build/test on `fe2`, then commit and push **there**.
- `gh` is authed as `jronminh`. Test prefix is `~/dn6` (`~/dn6/root`).
- No `sudo`/root anywhere; the box is small (low RAM).

## Rule

**Run one command at a time.** No chaining multiple commands, no parallel
jobs, no `-P` fan-out. Wait for the command to finish and read its output
before starting the next one. This isolates errors and keeps the small box
from being overloaded.
