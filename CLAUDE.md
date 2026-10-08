# CLAUDE.md — deb-native

Claude Code's project guidance. `AGENTS.md` is the owner's OpenCode working
file; its workflow may differ. This file is the guide for Claude Code sessions
in this repo.

## What this repo is

`deb-native` builds a **self-contained Debian `arm64` userland in a tarball**
(a "prefix"): its own glibc, `apt`, and files. A **poor host** installs it with
nothing but a POSIX shell and `tar`/`dd`/`sed` — no root, no `chroot`, no
namespaces, and the host's own tree untouched. Goal: a package reaches `dpkg`
status `ii` and its program runs by name, unprivileged.

## How it ships: build → ship → boot

There is **no build on the host**.

- **Build** (a build host, `scripts/build/` + `scripts/glibc/`): the runtime
  overlay (`scripts/build/build-overlay-glibc.sh` — `dn-trace`, one static
  binary with the syscall catalog embedded, plain-gcc), the glibc bundle (the two patches in
  `patches/`, applied by `scripts/glibc/dn-apply-glibc-patch.sh`), and the
  prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `.dn/` —
  `docs/spec/prefix.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `.dn/contract` without extracting, check it, extract, run `.dn/install.sh`.
  One install path, on a **poor host** (POSIX sh + toybox only).
- **Activate / boot**: the artifact's own `.dn/install.sh` (host shell) wires
  the session entry (the artifact is built for its final path, so there is no
  relocation); the host then boots the tree by running the contract's `entry`,
  which starts `dn-trace` and the prefix's init -- init completes `.dn/profile`
  itself on a first boot.

Run time is one policy in two places: dn-policy, called in-process by
dn-glibc (the fast path) and via `dn-trace`'s ptrace fallback
(`src/tracer/`). The old shim, maintainer-script rewriting and ELF editor
are gone; a `.deb` installs intact (`docs/spec/overlay.md`).

A package keeps its stock Debian `PT_INTERP`; the kernel never resolves it,
because every exec goes through the exec gate, which runs a glibc-dynamic
program through the runtime loader. No per-package `PT_INTERP` patching, no
`patchelf`, no `dn-elf`.

## Layout

- `src/` — `dn-policy/` (the shared policy library), `tracer/` (dn-trace, a
  pruned PRoot fork), `syscalls.tsv` (the syscall catalog).
- `scripts/build/`, `scripts/glibc/` — a build host.
- `scripts/host/`, `scripts/prefix/` — the host / the prefix.
- `patches/` — the glibc patches (Android base + dn-policy wiring).
  `tests/` `tools/` `docs/`.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource; build the idea the fastest testable
way. Don't deep-dive a broken attempt — try the shortest path to a working
result, and dig into the cause only when that path is blocked. **Verify the
change builds** before trusting or shipping it.

## Workflow

- Everything checked in (code, docs, commits) is in English, even when the
  conversation is in Vietnamese.
- **Docs state only the current truth** — no change narrative; history lives
  in git. After moving/renaming/deleting a doc or script, run
  `tools/check-repo.py`, `check-modules.py`, `check-interface.py`,
  `check-paths.py`.
- A long build is started with `nohup ... & disown` so it survives a session
  restart; check it with `ps`.

## Rule

**Run one command at a time.** No chaining, no parallel jobs. Wait for the
command to finish and read its output before starting the next.
