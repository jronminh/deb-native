# Design

<!-- template: templates/docs.template.md -->

How deb-native works: scope and philosophy, the self-contained prefix
artifact, the runtime layer, and fake root. Deeper mechanism write-ups live in
their own docs (listed below).

## Contents

- [Scope and design philosophy](#scope-and-design-philosophy)
- [The prefix artifact](#the-prefix-artifact)
- [The runtime layer](#the-runtime-layer)
- [Day-to-day commands](#day-to-day-commands)
- [Fake root](#fake-root)

## Related docs

- [`prefix-contract.md`](prefix-contract.md) — the `.dn/` contract a prefix
  artifact carries, and how a host installs it.
- [`prefix-layers.md`](prefix-layers.md) — core-ultra, core-deb, specialized.
- [`path-shim.md`](shim/path-shim.md) — the shim: design, verification,
  delivery.
- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) — the prefix's own glibc and its
  run-time prefix derivation.
- [`tracer.md`](tracer/tracer.md) — the syscall tracer for what the shim
  cannot see.
- [`android-platform.md`](../reference/android-platform.md) — the platform's
  enforcement gates, which bound what the runtime can do.
- [`runtime.md`](runtime.md) — the planned replacement for this doc's "Fake
  root" mechanism (and for the shim/tracer more broadly): `dn-policy` as the
  one source of truth, called from both the glibc fast path and the tracer.

## Scope and design philosophy

The goal is deliberately narrow: a Debian package should **install** — reach
`dpkg` status `ii` — and its program should **run by name**, unprivileged. Not
faithful Debian emulation, not isolation.

Everything heavy underneath is *real*: real `arm64`, real glibc (the prefix's
own build), real `apt`/`dpkg`, real ELF loading. The only thing faked is the
**layout** — that `/usr /etc /var /opt` exist. Detail is therefore spent only
at the **seams** that decide "installed" and "runs":

- the prefix (where files actually land) and the path interposition;
- the maintainer-script exec path, because `preinst`/`postinst` run outside
  the process we control;
- dpkg's own state, which is real bookkeeping, not a bluff.

Because this is an **adapter, not an emulator**, coverage is a named boundary
rather than a promise: paths that go around libc — static binaries, raw
`syscall()`, libc-internal `dlopen`, socket `sun_path` — are the syscall-level
tracer's job ([`tracer.md`](tracer/tracer.md)).

## The prefix artifact

A prefix is a **self-contained Debian userland**: its own glibc, `apt`/`dpkg`
and database, its own files, all under one root. It is built elsewhere and
shipped as a **tarball** carrying a `.dn/` contract; a host installs it with a
POSIX shell and `tar`/`dd`/`sed` only ([`prefix-contract.md`](prefix-contract.md)).
The host's own tree is never touched; deleting the prefix restores it exactly.

The design keeps two halves apart:

- **self-contained for installing**: `apt`/`dpkg` own their root, so Debian's
  normal rules (dependencies, Pre-Depends order, alternatives, diversions,
  upgrades) just work. The prefix ships with Debian's own `libc6`/`libc-bin`,
  `apt`, `dpkg` and a base, and its `libc6`/`libc-bin` are held so an upgrade
  cannot swap in a build that does not run here.
- **a guest for running**: the host may give an application no namespace
  "view" to fake paths with, so programs run through a runtime layer (the
  path shim, `dn-run`, the tracer) on the prefix's own glibc.

Install order: the host ships the artifact (`scripts/host/ship-prefix.sh`),
activates it (`.dn/install.sh`), and the prefix completes itself
(`.dn/bootstrap.sh`, installing `.dn/profile`). See
[`prefix-contract.md`](prefix-contract.md).

## The runtime layer

Three mechanisms, by what they can see:

- **Maintainer scripts** — the translator rewrites shebangs and interpreter
  paths at install (`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`), so a
  script's child runs under the prefix's own shell.
- **Dynamic glibc binaries** — the shim (`src/dn-shim.c`), an `LD_PRELOAD`
  library interposing path-taking libc functions (`/usr /etc /var /opt` →
  the prefix). Complete at its layer
  ([`shim/shim-coverage.md`](shim/shim-coverage.md)).
- **Syscall level** — the tracer (`src/tracer/`, `dn-trace`) for what the shim
  cannot see: static binaries, raw `syscall()`, libc-internal NSS reads. A
  translation whose interpreter is not on the host is routed through `dn-run`
  to the tracer.

Every translated program's `PT_INTERP` points at the prefix's own glibc
loader; it derives the live prefix from its own path at run time
([`dn-glibc-prefix.md`](dn-glibc-prefix.md)).

## Day-to-day commands

The prefix's own `apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg`,
`dpkg-query` are first on PATH inside a prefix session — Debian's packages, no
aliases.

- **`dn-adopt FILE...`** — make a glibc `arm64` program obtained outside apt
  run through the prefix: its interpreter becomes the prefix's own loader. The
  file is changed in place; anything else (static ones, scripts) is left
  alone.
- **Library path** — the prefix's loader finds the prefix's own libraries;
  a caller can still set `LD_LIBRARY_PATH=dir ./prog`, honoured as standard
  glibc does.

## Fake root

**Goal:** inside the prefix a program sees itself as root, as on a real Debian
where apt, dpkg and maintainer scripts run as root — `id` says `uid=0(root)`,
files show as owned by root, `chown`/`setuid` succeed, `USER`/`LOGNAME` are
`root`. Only the identity is faked: nothing gains a right it did not have, and
the host's own programs still see the real user.

**Status: released, no further investment planned in *this* mechanism.** It is
a stand-in, not a destination — the real fix for packages that need an actual
second identity is a services/sudo layer, not a stronger fake. Planned:
[`runtime.md`](runtime.md)'s `dn-policy` becomes the single source of truth
for fake root instead (principle 3: one policy, called from both the glibc
fast path and the tracer fallback, so the two can never disagree), replacing
both this shim/`dn-trace` mechanism and the parked
`patches/set-fakesyscalls-parked.patch` (the `setuid`/`setgid`/... "0" bucket,
[`android-platform.md`](../reference/android-platform.md)) — that patch bucket
is excluded *permanently* now, not pending a decision, since a second
independent fake-root at the glibc-patch layer would reintroduce the exact
disagreement risk principle 3 exists to prevent. Until `runtime.md` is
implemented, the mechanism below is what ships.

**Mechanism:** the shim (`src/dn-shim.c`) fakes
`get[e]uid`/`get[e]gid`/`getres[ug]id`/`getgroups` → `0`, `stat` ownership,
no-ops `chown`/`set*id`/`setgroups`/`initgroups`, and rewrites
`USER`/`LOGNAME` in the environ array; `dn-trace` does the same at syscall
exit for static programs, raw syscalls, and NSS.

**Escape hatch:** `DN_ID=user CMD` runs a command (and its children) with the
real identity instead, for programs that refuse to run as root.

**Cost:** free for normal programs (the shim does this inline). Under the
tracer, faking file owners means every `stat` round-trips through `dn-trace`;
`DN_ID=user` under the tracer gets the untraced speed back.
