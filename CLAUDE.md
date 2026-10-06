# CLAUDE.md — deb-native

Claude Code's own project guidance. `AGENTS.md` is the owner's OpenCode
working file; its workflow may differ. This file is the guide for Claude Code
sessions in this repo.

## What this repo is

`deb-native` runs real Debian **arm64** (`.deb`) packages inside **Termux** on
unrooted Android — no root, no `chroot`, no kernel namespaces. It fakes the
Debian *layout* (`/usr /etc /var /opt`) while everything else stays real: real
`aarch64`, real glibc (the prefix's own Android-patched build), real ELF
loading. Goal: a package reaches `dpkg` status `ii` and its program runs by
name, unprivileged.

## How it ships: build → ship → bootstrap

There is **no build on the target**.

- **Build** (a build host, `scripts/build/` + `scripts/glibc/`): the runtime
  overlay (`scripts/build/build-overlay-glibc.sh` — `dn-shim.so`, `dn-run`,
  `dn-trace`, `dn-elf`, plain-gcc glibc), the glibc bundle
  (`patches/dn-glibc-android.patch`), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `.dn/` —
  `docs/spec/prefix-contract.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the target's own shell): read
  `.dn/contract` without extracting, check it, extract, relocate. One install
  path, on a **poor host** (mksh + toybox only).
- **Activate / complete**: the artifact's own `.dn/install.sh` (host shell),
  then `.dn/bootstrap.sh` (the prefix's own shell, installing `.dn/profile`
  from the mirror). When bootstrap succeeds the prefix is ready.

Run time has three path mechanisms: maintainer-script rewrites
(`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`), the shim
(`src/dn-shim.c`), and the tracer (`src/tracer/`).

Every translated program's `PT_INTERP` points at the prefix's own fused glibc
loader (`$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`); it derives the
live prefix from its own path at run time and reads the shim from
`$DN/etc/ld.so.preload`. `patchelf` is gone — `dn-elf` is the ELF editor.
Prefer a launch-time env var / loader path over a static per-`.deb` ELF patch
when both would solve the same problem.

## Layout

- `src/` — the compiled runtime (`dn-shim.c`, `dn-run.c`, `dn-elf.c`,
  `dn-child.h`, `tracer/`, a pruned PRoot fork).
- `scripts/build/`, `scripts/glibc/` — a build host.
- `scripts/host/`, `scripts/prefix/` — the target / the prefix.
- `patches/` — the glibc Android patch. `tests/` `tools/` `docs/`.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource; build the idea the fastest testable
way. Don't deep-dive a broken attempt — try the shortest path to a working
result, and dig into the cause only when that path is blocked. **Verify the
change builds** before trusting or shipping it.

## Workflow

- Work happens **directly on this device** — Termux's own shell; this repo's
  working tree is the source of truth.
- Prefix is `/data/data/com.termux/files/deb-native` (`$DN`), beside Termux's
  own `usr/` and `home/`.
- `/tmp` is not writable for this session — use the scratchpad directory the
  environment block names.
- A long build is started with `nohup ... & disown` so it survives a session
  restart; check it with `ps`, not by assuming a backgrounded tool call is
  still tracked.
- Everything checked in (code, docs, commits) is in English, even when the
  conversation is in Vietnamese.
- **Docs state only the current truth** — no change narrative; history lives
  in git. After moving/renaming/deleting a doc or script, run
  `tools/check-repo.py`, `check-modules.py`, `check-interface.py`,
  `check-paths.py`.

## Rule

**Run one command at a time.** No chaining, no parallel jobs. Wait for the
command to finish and read its output before starting the next.
