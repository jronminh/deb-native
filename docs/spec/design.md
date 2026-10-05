# Design

<!-- template: templates/docs.template.md -->

How deb-native works, as it stands right now: scope, the 0.2.0
self-contained prefix (what's actually built), day-to-day commands, and
fake root. One doc for the live design, accumulated over releases rather
than forked per release — a section that gets superseded says so in
place instead of living on in a separate versioned file — but kept to
the current picture; deeper mechanism write-ups and superseded proposals
live in their own docs, listed below.

## Contents

- [Scope and design philosophy](#scope-and-design-philosophy)
- [The 0.2.0 pivot: a self-contained prefix](#the-020-pivot-a-self-contained-prefix)
- [Fake root (0.3.0)](#fake-root-030)

## Related docs

- [`path-shim.md`](shim/path-shim.md) — the dn-shim shim: design,
  verification, delivery mechanisms. Split out of this doc.
- [`native-reuse.md`](../notes/native-reuse.md) — native dependency reuse
  (`native-seed.sh`, since superseded). Split out of this doc.
- [`classic-design.md`](../notes/classic-design.md) — the pre-0.2.0 approach
  (plain `dpkg --instdir`, static per-binary wrappers, the services
  research) — superseded in large part by the 0.2.0 pivot below. Split
  out of this doc.
- [`prior-art.md`](../notes/prior-art.md) — sudo-less and proroot. Split out of
  this doc.
- [`android-platform.md`](../reference/android-platform.md) — the fake-root-entangled
  glibc patch bucket mentioned under "Fake root" below.

## Scope and design philosophy

The goal is deliberately narrow (the same shape as `sudo-less`): a Debian
package should **install** — reach `dpkg` status `ii` — and its program should
**run by name**, unprivileged. Not faithful Debian emulation, not isolation.

The design follows from that. Everything heavy underneath is *real*: real
aarch64, real glibc from Termux's side-install, Termux's real `apt`/`dpkg`,
real ELF loading. The only thing faked is the **layout** — that `/usr /etc /var
/opt` exist. Detail is therefore spent only at the **seams** that decide
"installed" and "runs":

- the prefix (where files actually land) and the path interposition;
- the maintainer-script exec path (`core/native/dn-launch.c`), because `preinst`/
  `postinst` run outside the process we control;
- dpkg's own state, which is real bookkeeping, not a bluff.

Everything in between can be ignored. And because this is an **adapter, not an
emulator**, coverage is a named boundary rather than a promise: paths that go
around libc — static binaries, raw `syscall()`, libc-internal `dlopen`, socket
`sun_path` — are out of scope until the syscall-level tracer
([`tracer.md`](tracer/tracer.md)) and `TODO.md` / issue #1.


## The 0.2.0 pivot: a self-contained prefix

Everything above is the classic, separate-prefix design (`dpkg --instdir`
straight into a plain directory, no database of its own). `dev-0.2.0`
proposed replacing it with a small, complete Debian system of its own:
its own `apt` and `dpkg`, its own database, its own `libc6` and Debian
base, installed at bootstrap — "just apt", the way it is on Debian.
Termux is never touched; deleting the prefix restores it exactly.

**Status: partly superseded.** `TODO.md`'s current roadmap keeps `arm64`
as a foreign architecture instead of this section's `aarch64` relabel.
What follows is kept as the record of the proposal, with the divergence
noted; the rest —
the self-contained-prefix idea itself, the stand-in packages, the launcher
wrappers — is what got built (`bootstrap/setup-apt-prefix.sh`).

This is the principle `sudo-less` uses on Debian: **apt and dpkg own their
root.** When the place packages live is apt/dpkg's own `/`, dpkg's normal
rules (dependencies, Pre-Depends order, alternatives, diversions, upgrades)
just work, and the glue between two package managers disappears. Taking
over Termux's own prefix would get that too, but one-way and unsafe; 0.2.0
gets it inside a sandbox.

The split stays as above: **self-contained for installing, a guest of
Termux for running.** Programs run through the runtime layer (path shim,
`dn-shell`, launchers, `dn-run`) on Termux's glibc, because Android gives
an app no namespace "view" to fake paths with.

### What the prefix contains from the start

| Package (prefix's own dpkg database) | What it is |
|---|---|
| `libc6` | stand-in: a real package whose files are links at Debian's libc paths into Termux's glibc (`$PREFIX/glibc/lib`). Debian's own `libc6` is killed by Android's seccomp filter at startup; Termux's glibc is the same library patched for Android at source level. Version = Termux's glibc version. |
| `dpkg`, `apt` | stand-ins for Termux's own `dpkg`/`apt`, versioned like Termux's, so `Depends: dpkg (>= ...)` is satisfied |
| `mawk`, `base-files`, `base-passwd`, `dash`, `debianutils`, `debconf`, `cdebconf`, `openssl`, `ca-certificates` | Debian's own, translated at install, then **held** |

A separate database has no name clashes with Termux, so `dash`, `openssl`
and `ca-certificates` are Debian's own packages. Only
`libc6`, `dpkg` and `apt` are stand-ins.

### apt and dpkg: Termux's own, through launchers (decided, built)

The prefix uses the `apt`/`dpkg` Termux already has: no build toolchain,
no rebuilt packages, lowest requirements. Termux updates to them carry
through.

- **`$DN/bin/apt*`, `$DN/bin/dpkg*`** are small launchers that run Termux's
  binaries with the prefix given explicitly: `APT_CONFIG=$DN/etc/apt.conf`,
  `--admindir=$DN/var/lib/dpkg`, `--instdir=$DN`. Explicit, never
  `DPKG_ROOT` alone: Termux's tools have Termux's paths compiled in, and
  under `DPKG_ROOT` they prepend the prefix to a path that already contains
  one (the doubled-path bug class, below).
- **dpkg's helpers** compute paths from `DPKG_ROOT` by themselves when a
  maintainer script calls them, so each gets a wrapper forcing its own
  paths (each was root-caused with `strace`):
  - `update-alternatives`: `--altdir`/`--admindir`, and `--log
    /var/log/alternatives.log` with `DPKG_ROOT` set -- it joins
    `DPKG_ROOT` onto an explicit `--log` too.
  - `dpkg-divert`: `--admindir`/`--instdir` (it opened
    `$DN$PREFIX/var/lib/dpkg/diversions`).
  - `dpkg-statoverride`: no-op (unprivileged, no ownership).
  - `dpkg-trigger`: to check when a trigger-using package comes through.

### Layout: Debian's own, nested

`$DN/usr/bin`, `$DN/etc`, ... exactly as on Debian, merged-`/usr`
links from `base-files` included. A prefix of our own has no reason to
flatten `usr/` (only taking over Termux's flat `$PREFIX` would). The shim
keeps mapping `/usr` -> `$DN/usr`.

### Architecture: proposed relabel to `aarch64` (not what shipped)

Termux's dpkg calls this CPU `aarch64` (compiled in); Debian calls it
`arm64`. This section proposed rewriting the Debian index and every
package's control file (`Architecture: arm64`/`all` -> `aarch64`) after
each `apt update`, so dpkg would see native packages: no
`--force-architecture`, no foreign architecture, `all` no longer special.
**`TODO.md` kept `arm64` as a foreign architecture instead** — `arm64`
stays foreign, `dn-debian-index.sh` only rewrites `all` -> `arm64`, and
`--force-architecture` is still in use where the classic design needed it
(`../log/findings/first-working-prototype.md` calls that "not a real
fix", but the relabel was never built to replace it).

### Install pipeline

Phase A (built): translation on the device, in the prefix's apt hooks:

| Hook | Step |
|---|---|
| `DPkg::Pre-Install-Pkgs` | per `.deb`, via `dn-hook-pre.sh` -> `dn-translate-deb.sh`: control relabel; ELFs repointed at the `libc6` stand-in (`$DN/usr/lib/ld-linux-aarch64.so.1`, `RUNPATH` `$DN/usr/lib/aarch64-linux-gnu` first, shared libraries too) before any maintainer script runs; `core/custom/<package>.sh` fixes; maintainer-script shebangs -> `dn-shell` (`patch-scripts-tree.sh`) |
| `DPkg::Post-Invoke` | alternatives links made relative at once (`dn-fix-alternatives.sh`); new packages' absolute symlinks made relative; launchers for their programs (and for alternatives links to them); stale launchers dropped |

Phase B (not built): the same translation at **repo build time**
([`deb-native-repo`](https://github.com/jronminh/deb-native-repo),
private): packages arrive translated and **signed** (ending
`[trusted=yes]`), hashes match what gets installed, the device hooks stay
only as a fallback for packages not in the repo.

### What went away from the classic design

- `native-seed.sh` (stub database entries): replaced by the real `libc6`
  stand-in and Debian's own libraries.
- `patch-elfs.sh` after install (`grun --configure` on the whole prefix):
  ELFs are repointed in the package, before any script runs.
- `apt-install.sh`'s one-package-at-a-time loop: it exists because patching
  had to happen outside apt; with hooks, plain `apt install` keeps dpkg's
  own Pre-Depends ordering.

### What stays

- The runtime layer: shim, `dn-shell`/`dn-perl`, `dn-run`, launchers,
  `termux-dn-doctor`.
- Routing ("Termux wins") -- superseded: the Debian userland is the default
  session, so `apt`/`dpkg` are the prefix's by PATH; Termux's are `pkg`
  in a Termux shell (`termux-shell`). See `docs/spec/userlands.md`.

### Day-to-day commands

The Debian userland is the default session (`docs/spec/userlands.md`):
opening Termux lands in Debian, not in Termux's own shell.

- `apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg`, `dpkg-query` are
  **the prefix's** (Debian's packages), by PATH -- no aliases.
- **`termux-shell`** -- open a clean, nested Termux shell: there
  `pkg`, `termux-apt` and `termux-dpkg` are Termux's, and `exit` returns
  to the Debian userland. `pkg` typed in the userland is refused (a guard).
- **`dn-shell`** -- from a Termux shell, enter the Debian userland again
  (the reverse crossing). The Debian prompt is red (`~ # `), Termux's
  green (`~ $ `).
- **`dn-adopt FILE...`** -- make a glibc arm64 program obtained outside
  apt (a release download, a direct installer's binary) run through the
  prefix: its interpreter becomes the prefix's own fused glibc loader. The
  file is changed in place; anything else (Termux's own programs, static
  ones, scripts) is left alone.
- **Library path** -- the prefix's fused glibc loader finds the prefix's
  own libraries through `$DN/usr/etc/ld.so.cache` (built by our
  `ldconfig`), covering a program's whole transitive load graph with no
  per-`.deb` `RUNPATH` patch (`docs/log/findings/patchelf-et-exec-runpath.md`
  for why). A caller can still set `LD_LIBRARY_PATH=dir ./prog` for its own
  libraries; the loader honours it as standard glibc does.
  Worked example: `docs/guides/gcc-glibc-dev.md`.


## Fake root (0.3.0)

**Goal:** inside the prefix a program sees itself as root, as on a real
Debian where apt, dpkg and maintainer scripts run as root -- `id` says
`uid=0(root)`, files show as owned by root, `chown`/`setuid` succeed,
`USER`/`LOGNAME` are `root`. Only the identity is faked: nothing gains a
right it did not have, and Termux's own programs still see the real user.

**Status: released, no further investment planned** (reconsidered
2026-10-01, `TODO.md`'s "0.3.0: fake root"). It is a stand-in, not a
destination -- the real fix for packages that need an actual second
identity is a services/sudo layer (`TODO.md`), not a stronger fake. Kept
exactly as released; the parked `set-fakesyscalls-parked.patch` (0.5.0's
glibc patch, the `setuid`/`setgid`/... "0" bucket,
[`android-platform.md`](../reference/android-platform.md)) stays unapplied for the
same reason -- nothing about fake-root is being extended.

**Mechanism:** the shim (`core/native/dn-shim.c`) fakes
`get[e]uid`/`get[e]gid`/`getres[ug]id`/`getgroups` -> `0`, `stat`
ownership, no-ops `chown`/`set*id`/`setgroups`/`initgroups`, and rewrites
`USER`/`LOGNAME` in the environ array; `dn-trace` does the same at syscall
exit for static programs, raw syscalls, and NSS.

**Escape hatch:** `DN_ID=user CMD` runs a command (and its children) with
the real identity instead, for programs that refuse to run as root
(`postgres`, Chromium's sandbox).

**Cost:** free for normal programs (the shim does this inline). Under the
tracer (static programs, raw syscalls), faking file owners means every
`stat` round-trips through `dn-trace` instead of the kernel directly: a
stat-heavy `find` took 727 ms instead of 443 ms on the test phone.
`DN_ID=user` under the tracer gets the untraced speed back, since it skips
the identity fakery entirely.
