# AGENTS.md — deb-native

## What this repo is

`deb-native` installs and runs real Debian **arm64** (`.deb`) packages inside
**Termux** on unrooted Android — **no root, no `chroot`, no kernel
namespaces** (user namespaces are off kernel-wide here). It fakes the Debian
*layout* (`/usr /etc /var /opt`) while everything else stays real: real
`aarch64`, real glibc (Termux's `$PREFIX/glibc` side-install), Termux's own
`apt`/`dpkg`, real ELF loading.

Goal: a package reaches `dpkg` status **`ii`** and its program runs **by
name**, unprivileged. "Install and run, not emulate" — no isolation.

Three path mechanisms:

- **Maintainer scripts** — plain-text path rewrite before `dpkg` runs them
  (`scripts/patch-maintainer-scripts.sh`).
- **Dynamic glibc binaries** — `native/path-redirect.c`, an `LD_PRELOAD` shim
  interposing path-taking libc functions (`/usr /etc /var /opt` →
  `$INSTDIR`). **This layer is complete** — see `docs/shim-coverage.md`.
- **Syscall level** — for what the shim cannot see (static binaries, inline
  `svc #0`, explicit `syscall()`, libc-internal NSS reads). Today this is
  `proot` (fallback, wired in `native/dn-run.c`); it is being replaced by
  **fork-lite**, our own reduced proot, in `tracer/`.

`sudo-less` (`~/sudo-less`) is the same idea on a real Debian host using a
mount-namespace "view"; the view is impossible here, so these mechanisms
replace the view's path-resolution job.

## Docs map (read these before changing direction)

- `docs/standard.md` — package scope (Debian-section based).
- `docs/shim-coverage.md` + `docs/coverage/` — measured libc-shim coverage
  against a 258-package corpus; what is left.
- `docs/syscall-boundary.md` — the cases libc interposition cannot reach.
- `docs/direct-usage.md` — the living investigation + **fork-lite plan**.
- `docs/bind-only.md` — what to strip to make fork-lite a bind-only tracer
  (`canonicalize` → prefix rewrite): safe/keep/replace + whitelist + risks.
- `docs/design.md`, `docs/findings.md`, `docs/vs-sudo-less.md`.
- `tracer/README.md` — fork-lite provenance, build, prune status.
- `TODO.md` — roadmap.

## Current state (2026-09-26)

- Libc shim finished at its layer; `tests/shim-libc/run.sh` passes (46
  rewrites). NSS/raw-syscall/static are the documented boundary.
- **fork-lite** (`tracer/`): proot base imported and pruned — extensions
  removed (framework kept) and made AArch64-only. It **builds on `fe2`**
  (`make CC=clang`, needs `libtalloc`) and a static binary reads through a
  `-b` bind. **Next: replace `cli/` with our `dn-trace` binder** (bind
  `$INSTDIR` over `/usr /etc /var /opt`, handle missing paths), then point
  `dn-run.c` at it and test static / `abbtr` / NSS.
- Corpus on the phone: `~/debcorpus/` (Packages.gz, `debs/`, `root/`).

## Layout

- `native/` — `path-redirect.c` (shim), `dn-launch.c`, `dn-run.c`.
- `tracer/` — fork-lite (reduced proot), GPLv2+ headers kept.
- `tests/shim-libc/` — on-device shim smoke test.
- `scripts/` — bootstrap, apt/dpkg wiring, patching, launchers, surveys,
  `scan-libc-symbols.sh`, `scan-direct-syscalls.py`, `scope-sample.py`.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the Termux
  user with the toolchain). It pushes to GitHub `main`.
- A local copy may be **stale**. Edit locally, `scp`/`tar` to `fe2`, build and
  test on `fe2`, then commit and push **there**. `gh` is authed as `jronminh`.
- Test prefix `~/dn6` (`~/dn6/root`). No `sudo`/root anywhere; the box is
  small (low RAM).
- Persisted files (code, docs, commits) are in **English**.

## Rule

**Run one command at a time.** No chaining, no parallel jobs, no `-P`
fan-out. Wait for the command to finish and read its output before starting
the next. This isolates errors and keeps the small box from being
overloaded.
