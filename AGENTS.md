# AGENTS.md — deb-native

## What this repo is

`deb-native` builds a **self-contained Debian `arm64` userland in a tarball**
(a "prefix"): its own glibc, `apt`, and files. A **poor host** installs it
with nothing but a POSIX shell and `tar`/`dd`/`sed` (toybox is enough) — no
root, no `chroot`, no namespaces, and the host's own tree untouched. Goal: a
package reaches `dpkg` status **`ii`** and its program runs **by name**,
unprivileged. "Install and run, not emulate" — no isolation.

## How it ships: build → ship → bootstrap

There is **no build on the host**.

- **Build** (a build host: `scripts/build/`, `scripts/glibc/`): the runtime
  overlay (`scripts/build/build-overlay-glibc.sh` — `dn-shim.so`, `dn-run`,
  `dn-trace`, `dn-elf`, plain-gcc glibc), the glibc bundle
  (`patches/dn-glibc-android.patch`), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `.dn/` —
  `docs/spec/prefix-contract.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `.dn/contract` without extracting, extract, run `.dn/install.sh`. This is the
  one install path, on a **poor host** (POSIX sh + toybox only).
- **Activate / complete**: the artifact's own `.dn/install.sh` (host shell)
  relocates the prefix -- the loader runs the artifact's `dn-elf`, which
  repoints every ELF -- and wires the session entry; then `.dn/bootstrap.sh`
  (the prefix's own shell) installs `.dn/profile`. When bootstrap succeeds the
  prefix is ready.

Run time has three path mechanisms: maintainer-script rewrites
(`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`), the shim
(`src/dn-shim.c`), and the tracer (`src/tracer/`).

## Layout

- `src/` — the compiled runtime: `dn-shim.c`, `dn-run.c`, `dn-elf.c`,
  `dn-child.h`, `tracer/` (a pruned PRoot fork).
- `scripts/build/` — build host: overlay, artifact assembly, packaging.
- `scripts/glibc/` — build host: apply the glibc patch, package `libc6` /
  `libc-bin`.
- `scripts/host/` — the host's own shell (poor host): ship, bootstrap,
  install (activation + relocation via the artifact's loader/dn-elf), hooks,
  adopt, update.
- `scripts/prefix/` — run by the prefix (its own glibc): apt hooks, package
  translation, per-package fixes; `interface.tsv` is the module surface.
- `patches/` — the glibc Android patch.
- `tests/` — on-device smoke tests; `tools/` — repo checks + helpers;
  `docs/` — spec, reference, notes.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the build host
  with the toolchain). It pushes to GitHub. A local copy may be stale: edit
  locally, `scp`/`tar` to `fe2`, build and test there, then commit and push
  there. `gh` is authed as `jronminh`.
- No `sudo`/root anywhere. Persisted files (code, docs, commits) are in
  **English**.
- **Docs state only the current truth.** No history narrative; history lives
  in git.
- After moving/renaming/deleting a doc or a script, run
  `tools/check-repo.py` (links, anchors, reachability), plus
  `check-modules.py`, `check-interface.py` and `check-paths.py`.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource. Build the idea the **fastest testable
way**; dig into a failure only when the shortest path is actually blocked.

- Choose the approach that directly exercises the idea.
- **Verify the change builds** before trusting or shipping it.

## Rule

**Run one command at a time.** No chaining, no parallel jobs, no `-P` fan-out.
Wait for the command to finish and read its output before starting the next.
