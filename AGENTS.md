# AGENTS.md — deb-native

## What this repo is

`deb-native` builds a **self-contained Debian `arm64` userland in a tarball**
(a "prefix"): its own glibc, `apt`, and files. A **poor host** installs it
with nothing but a POSIX shell and `tar`/`dd`/`sed` (toybox is enough) — no
root, no `chroot`, no namespaces, and the host's own tree untouched. Goal: a
package reaches `dpkg` status **`ii`** and its program runs **by name**,
unprivileged. "Install and run, not emulate" — no isolation.

## How it ships: build → ship → boot

There is **no build on the host**.

- **Build** (a build host: `scripts/build/`, `scripts/glibc/`): the runtime
  overlay (`scripts/build/build-overlay-glibc.sh` — `dn-trace`, one static
  binary with the syscall catalog embedded, plain-gcc), the glibc bundle (the two patches in
  `patches/`, applied by `scripts/glibc/dn-apply-glibc-patch.sh`), and the
  prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `.dn/` —
  `docs/spec/prefix.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `.dn/contract` without extracting, extract, run `.dn/install.sh`. This is the
  one install path, on a **poor host** (POSIX sh + toybox only).
- **Activate / boot**: the artifact's own `.dn/install.sh` (host shell) wires
  the session entry; the host then boots the tree by running the contract's
  `entry`, which starts `dn-trace` and the prefix's init -- `dn-trace` derives
  the prefix root from its own location, so the artifact is relocatable; init
  completes `.dn/profile` itself on a first boot.

Run time is one policy in two places: dn-policy, called in-process by
dn-glibc (the fast path) and via `dn-trace`'s ptrace fallback
(`src/tracer/`). The old shim, maintainer-script rewriting and ELF editor
are gone; a `.deb` installs intact (`docs/spec/overlay.md`).

## Layout

- `src/` — `dn-policy/` (the shared policy library), `tracer/` (dn-trace, a
  pruned PRoot fork), `syscalls.tsv` (the syscall catalog).
- `scripts/build/` — build host: overlay, artifact assembly, packaging.
- `scripts/glibc/` — build host: apply the glibc patch, package `libc6` /
  `libc-bin`.
- `scripts/host/` — the host's own shell (poor host): ship,
  install (activation), update.
- `scripts/prefix/` — run by the prefix (its own glibc); `interface.tsv` is
  the module surface.
- `patches/` — the glibc patches: the Android base + the dn-policy wiring.
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
