# AGENTS.md — deb-native

## What this repo is

`deb-native` runs real Debian **arm64** (`.deb`) packages inside **Termux** on
unrooted Android — **no root, no `chroot`, no kernel namespaces**. It fakes the
Debian *layout* (`/usr /etc /var /opt`) while everything else is real: real
`aarch64`, real glibc (the prefix's own Android-patched build), real ELF
loading. Goal: a package reaches `dpkg` status **`ii`** and its program runs
**by name**, unprivileged. "Install and run, not emulate" — no isolation.

## How it ships: build → ship → bootstrap

There is **no build on the target**.

- **Build** (a build host: `scripts/build/`, `scripts/glibc/`): the glibc
  bundle (`patches/dn-glibc-android.patch`), the runtime overlay
  (`scripts/build/build-overlay-glibc.sh` — `dn-shim.so`, `dn-run`, `dn-trace`,
  `dn-elf`, all plain-gcc glibc), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which applies the build
  invariants and writes `.dn/` — `docs/spec/prefix-contract.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the target's own shell): read
  `.dn/contract` without extracting, extract, relocate. This is the one install
  path, on a **poor host** (mksh + toybox only).
- **Activate / complete**: the artifact's own `.dn/install.sh` (host shell)
  then `.dn/bootstrap.sh` (the prefix's own shell, installing `.dn/profile`
  from the mirror). When bootstrap succeeds the prefix is ready.

Run time has three path mechanisms: maintainer-script rewrites
(`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`), the shim
(`src/dn-shim.c`), and the tracer (`src/tracer/`).

## Layout

- `src/` — the compiled runtime: `dn-shim.c`, `dn-run.c`, `dn-elf.c`,
  `dn-child.h`, `tracer/` (a pruned PRoot fork).
- `scripts/build/` — build host: overlay, artifact assembly, packaging.
- `scripts/glibc/` — build host: apply the glibc patch, package `libc6` /
  `libc-bin`.
- `scripts/host/` — the target's own shell (poor host): ship, bootstrap,
  install (activation), relocate, hooks, adopt, update.
- `scripts/prefix/` — run by the prefix (its own glibc): apt hooks, package
  translation, per-package fixes; `interface.tsv` is the module surface.
- `patches/` — the glibc Android patch.
- `tests/` — on-device smoke tests; `tools/` — repo checks + helpers;
  `docs/` — spec, reference, notes.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the Termux
  user with the toolchain). It pushes to GitHub. A local copy may be stale:
  edit locally, `scp`/`tar` to `fe2`, build and test there, then commit and
  push there. `gh` is authed as `jronminh`.
- No `sudo`/root anywhere. Persisted files (code, docs, commits) are in
  **English**.
- **Docs state only the current truth.** No "was X", no superseded banner, no
  change narrative; history lives in git.
- After moving/renaming/deleting a doc or a script, run
  `tools/check-repo.py` (links, anchors, reachability), plus
  `check-modules.py`, `check-interface.py` and `check-paths.py`.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource. Build the idea the **fastest testable
way**; dig into a failure only when the shortest path is actually blocked.

- Choose the approach that directly exercises the idea.
- **Verify the change builds** (clean tree) before trusting or shipping it.

## Rule

**Run one command at a time.** No chaining, no parallel jobs, no `-P` fan-out.
Wait for the command to finish and read its output before starting the next.
This isolates errors and keeps the small box from being overloaded.
