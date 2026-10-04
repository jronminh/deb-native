# CLAUDE.md — deb-native

Claude Code's own project guidance. `AGENTS.md` in this repo is the
project owner's personal OpenCode working file, unrelated to Claude Code
sessions — don't read it as instructions here; it describes a different
(older) workflow (`ssh fe2`, prefix `~/dn6`) that doesn't apply.

## What this repo is

`deb-native` installs and runs real Debian **arm64** (`.deb`) packages
inside **Termux** on unrooted Android — no root, no `chroot`, no kernel
namespaces (user namespaces are off kernel-wide on this device). It fakes
the Debian *layout* (`/usr /etc /var /opt`) while everything else stays
real: real `aarch64`, real glibc, Termux's own `apt`/`dpkg`, real ELF
loading. Goal: a package reaches `dpkg` status `ii` and its program runs
by name, unprivileged.

Three path mechanisms:
- **Maintainer scripts** — plain-text path rewrite (`scripts/install/patch-scripts-tree.sh`).
- **Dynamic glibc binaries** — `native/path-redirect.c`, an `LD_PRELOAD`
  shim interposing path-taking libc functions. Complete at its layer —
  `docs/spec/shim/shim-coverage.md`.
- **Syscall level / can't-be-shimmed cases** — `tracer/` (`dn-trace`, a
  reduced fork of PRoot's ptrace core), wired via `native/dn-run.c`. Done,
  not a TODO — replaced the old `proot` fallback in 0.2.3.

Every translated program's `PT_INTERP` points at the prefix's own fused
glibc loader (`$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`) — the
kernel has no other way to invoke a glibc program here, since Android has
no `/lib/ld-linux-aarch64.so.1`. That loader is Debian's glibc source with
this project's Android compatibility patches (the ten-file swap,
`docs/spec/deploy.md`); it derives the live prefix from its own path at run
time, reads the path shim from `$DN/etc/ld.so.preload`, and the prefix's
library dirs from `$DN/usr/etc/ld.so.cache`. The old `native/ld-dn.c`
trampoline is retired. Prefer a launch-time env var / loader path over a
static per-`.deb` ELF patch when both would solve the same problem — one
code path, no risk of `patchelf` miscomputing a binary's layout (see
`docs/log/findings/patchelf-et-exec-runpath.md`).

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

Every directory that holds more than one or two files has its own
`README.md` indexing what's in it and what each file is for — `scripts/`
and `docs/` each have one at every level (`scripts/install/README.md`,
`docs/spec/README.md`, ...), and so do `native/`, `tests/`,
`third_party/glibc-android-patches/`, `custom/`, and `tracer/`. Read the
relevant one before working in a directory, or before guessing a file's
purpose from its name alone — it's cheaper and more current than
re-deriving it from the code, and these are the first thing to update
when a file moves or a new one is added. Start from
[`docs/README.md`](docs/README.md) (which doc kind goes where) and
[`scripts/README.md`](scripts/README.md) (which lifecycle stage a script
belongs to) for the two biggest trees.

Every `README.md` and every content doc under `docs/` follows a stable
template in [`templates/`](templates/) (`readme.template.md`,
`docs.template.md`); copy the matching template instead of improvising a
layout when adding a new one. The how-to lives as an HTML comment inside
each template, and a one-line `<!-- template: ... -->` marks each finished
file, so the rendered doc stays clean for readers.

**Docs state only the current truth.** From 0.6.0+s.1 no doc under `docs/`
records history — no "was X", no superseded banner, no change narrative;
the only history is `docs/log/`. A retired mechanism's spec moves to
`docs/log/` and the live specs are rewritten to the new truth
([`docs/README.md`](docs/README.md)).

After moving, renaming, or deleting a doc or a script, run
`scripts/tools/check-repo.py` — it catches broken markdown links, broken
table-of-contents anchors, and scripts nothing calls any more (reported,
not failed on, since some of that is deliberate: `bench/`, `survey/`,
and similar are meant to be run by hand). This project has found and
removed the same class of dead file by hand several times; use the
script instead of re-deriving the check.

`docs/spec/design.md` is the live design doc (scope, the 0.2.0
self-contained prefix, day-to-day commands, fake root) — read it before
changing direction; its own intro points out to the deeper or superseded
write-ups split out of it. `docs/log/findings/` is the chronological
engineering log, one entry per file. `TODO.md` is the roadmap and
current-status source of truth.

## Current state

- Libc shim: complete at its layer.
- Tracer (`dn-trace`): built, wired, NSS + direct-syscall routing done.
- **0.5.0 "own glibc"**: patch forked (147 file-diffs,
  `third_party/glibc-android-patches/dn-glibc-android.patch`), fake-root's
  `"0"`-bucket entries deliberately parked pending a decision. First
  on-device `configure`+`make` attempt is what unblocked the long-standing
  "clang can't build glibc" wall — see `docs/log/android-seccomp-audit.md`'s
  last section for the live log of what broke and why (kernel headers,
  `PATH` shadowing Termux's own coreutils with the prefix's, a raw
  `clone3.S` syscall stub needing deletion to match `disabled-syscall.h`).

## Workflow

- Work happens **directly on this device** (no `ssh`, no remote host) —
  Termux's own shell, this repo's working tree is the source of truth.
- Prefix is `/data/data/com.termux/files/home/.dn` (`$DN`), fixed, not
  `~/dn6`.
- `/tmp` is not writable for this session — use the scratchpad directory
  the environment block names, not `/tmp`.
- A long build (e.g. the glibc build itself) is started with
  `nohup ... & disown` so it survives a session restart; check on it with
  `ps aux | grep`, not by assuming a backgrounded tool call is still
  tracked — a fresh session has no memory of prior background task IDs.
- Everything checked in (code, docs, commits) is in English, even when the
  conversation with the user is in Vietnamese.
