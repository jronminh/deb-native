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

## How it ships: build → ship → bootstrap

There is **no build on the host**.

- **Build** (a build host, `scripts/build/` + `scripts/glibc/`): the runtime
  overlay (`scripts/build/build-overlay-glibc.sh` — `dn-shim.so`, `dn-run`,
  `dn-trace`, `dn-elf`, plain-gcc glibc), the glibc bundle
  (`patches/dn-glibc-android.patch`), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `.dn/` —
  `docs/spec/prefix-contract.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `.dn/contract` without extracting, check it, extract, run `.dn/install.sh`.
  One install path, on a **poor host** (POSIX sh + toybox only).
- **Activate / complete**: the artifact's own `.dn/install.sh` (host shell)
  relocates the prefix -- the loader runs the artifact's `dn-elf`, which
  repoints every ELF -- and wires the session entry; then `.dn/bootstrap.sh`
  (the prefix's own shell) installs `.dn/profile`. When bootstrap succeeds the
  prefix is ready.

Run time has three path mechanisms: maintainer-script rewrites
(`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`), the shim
(`src/dn-shim.c`), and the tracer (`src/tracer/`).

Every translated program's `PT_INTERP` points at the prefix's own fused glibc
loader; it derives the live prefix from its own path at run time and reads the
shim from `etc/ld.so.preload`. `patchelf` is gone — `dn-elf` is the ELF
editor. Prefer a launch-time env var / loader path over a static per-`.deb`
ELF patch when both would solve the same problem.

## Layout

- `src/` — the compiled runtime (`dn-shim.c`, `dn-run.c`, `dn-elf.c`,
  `dn-child.h`, `tracer/`, a pruned PRoot fork).
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
