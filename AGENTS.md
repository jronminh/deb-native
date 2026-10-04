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
  (`scripts/install/patch-scripts-tree.sh`).
- **Dynamic glibc binaries** — `native/path-redirect.c`, an `LD_PRELOAD` shim
  interposing path-taking libc functions (`/usr /etc /var /opt` →
  `$INSTDIR`). **This layer is complete** — see `docs/spec/shim/shim-coverage.md`.
- **Syscall level** — for what the shim cannot see (static binaries, inline
  `svc #0`, explicit `syscall()`, libc-internal NSS reads). Today this is
  `proot` (fallback, wired in `native/dn-run.c`); it is being replaced by
  **fork-lite**, our own reduced proot, in `tracer/`.

`sudo-less` (`~/sudo-less`) is the same idea on a real Debian host using a
mount-namespace "view"; the view is impossible here, so these mechanisms
replace the view's path-resolution job.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource. When he has an idea, the goal is to
get it built and testable the **fastest possible way** — optimize for the
idea shipping soonest, not for understanding every failure.

- Don't deep-dive a broken attempt across many branches. Try the shortest
  path to a working result first; dig into the cause only when that path is
  actually blocked.
- Choose the approach that directly exercises the idea. Cost hours once:
  building on CI with the hardcoded `.dn` prefix when the idea needed a
  **dynamic prefix** — the build passed but could never test the idea.
- **Verify the patch builds** (clean tree, CI) before trusting or shipping
  it. Using a patch that was never build-checked is what turned one idea
  into a multi-hour detour.

## Repository layout: read the directory's own README.md first

`scripts/` and `docs/` each have a `README.md` at every level
(`scripts/install/README.md`, `docs/spec/README.md`, ...) indexing what's
in that directory; so do `native/`, `tests/`,
`third_party/glibc-android-patches/`, `custom/`, and `tracer/`. Read the
relevant one before working in a directory or guessing a file's purpose
from its name. Start from `docs/README.md` and `scripts/README.md`.
Every `README.md` and content doc follows a stable template in
`templates/` (`readme.template.md`, `docs.template.md`) — copy it instead
of improvising a layout when adding a new one. After moving/renaming/
deleting a doc or a script, run `scripts/tools/check-repo.py` (broken
links, broken ToC anchors, scripts nothing calls any more).

`docs/spec/design.md` is the live design doc (scope, the 0.2.0
self-contained prefix, day-to-day commands, fake root) — read it before
changing direction; its intro points out to `path-shim.md`,
`native-reuse.md`, `classic-design.md` and `prior-art.md` for depth or
history. `docs/log/findings/` is the chronological engineering log,
one entry per file.
`docs/reference/android-platform.md` has the Android enforcement-gate taxonomy
(seccomp/capability/SELinux) — read before scoping 0.5.0 work.
`tracer/README.md` has fork-lite's provenance, build, and prune status.
`TODO.md` is the roadmap.

## Current state (2026-09-26)

- Libc shim finished at its layer; `tests/shim-libc/run.sh` passes (46
  rewrites). **NSS (case 2) is now solved** by the tracer route plus a bind of
  the prefix `/etc` over Termux glibc's sysconfdir (`dn-run.c` routes a
  glibc+NSS ELF through `dn-trace`; `tests/tracer-nss/run.sh` passes).
  **Direct-syscall binaries (cases 3/4) are routed too**: `scan-direct-syscalls.py
  --trace-list` + `make-launchers.sh` tag them `dn-run --trace` at install time;
  static binaries use the tracer as before.
- **fork-lite** (`tracer/`): proot base imported and pruned — extensions
  removed (framework kept) and made AArch64-only. It **builds on `fe2`**
  (`make CC=clang`, needs `libtalloc`) and a static binary reads through a
  `-b` bind. **Bind-only fast path landed** (`docs/spec/tracer/bind-only.md`,
  `scripts/install/normalize-symlinks.sh`, ~1.6x stat-dense; `PROOT_NO_BIND_ONLY=1`
  reverts). **Next: replace `cli/` with our `dn-trace` binder** (bind
  `$INSTDIR` over `/usr /etc /var /opt`, handle missing paths), then point
  `dn-run.c` at it and test static / `abbtr` / NSS.
- Corpus on the phone: `~/debcorpus/` (Packages.gz, `debs/`, `root/`).

## Layout

See each directory's own `README.md` (`native/README.md`,
`tracer/README.md`, `tests/README.md`, `scripts/README.md` and its
per-subdirectory ones) — kept current there, not duplicated here.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the Termux
  user with the toolchain). It pushes to GitHub `main`.
- A local copy may be **stale**. Edit locally, `scp`/`tar` to `fe2`, build and
  test on `fe2`, then commit and push **there**. `gh` is authed as `jronminh`.
- Test prefix `~/dn6`. No `sudo`/root anywhere; the box is
  small (low RAM).
- Persisted files (code, docs, commits) are in **English**.
- **Docs state only the current truth.** From 0.6.0+s.1 no doc records
  history — no "was X", no superseded banner, no change narrative. The only
  history is `docs/log/` (`docs/README.md` has the rule). A retired
  mechanism's spec moves to `docs/log/`; live docs are rewritten to the new
  truth.

## Rule

**Run one command at a time.** No chaining, no parallel jobs, no `-P`
fan-out. Wait for the command to finish and read its output before starting
the next. This isolates errors and keeps the small box from being
overloaded.
