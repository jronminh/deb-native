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
  `docs/spec/shim-coverage.md`.
- **Syscall level / can't-be-shimmed cases** — `tracer/` (`dn-trace`, a
  reduced fork of PRoot's ptrace core), wired via `native/dn-run.c`. Done,
  not a TODO — replaced the old `proot` fallback in 0.2.3.

`native/ld-dn.c` is the ELF interpreter every translated program's
`PT_INTERP` points at (the kernel has no other way to invoke a glibc
program here — Android has no `/lib/ld-linux-aarch64.so.1`). It builds a
per-launch environment (`LD_PRELOAD`, `DN_INSTDIR`, `LD_LIBRARY_PATH`,
`COMPILER_PATH`) before handing off to glibc's real loader. Prefer adding
a launch-time env var here over a static per-`.deb` ELF patch when both
would solve the same problem — one code path, no risk of `patchelf`
miscomputing a binary's layout (see `docs/log/findings.md`, "patchelf
corrupting an `ET_EXEC` binary's program headers", 2026-09-30).

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

`docs/spec/design.md` is the live design doc (scope, the 0.2.0
self-contained prefix, day-to-day commands, fake root) — read it before
changing direction; its own intro points out to the deeper or superseded
write-ups split out of it. `docs/log/findings.md` is the chronological
engineering log. `TODO.md` is the roadmap and current-status source of
truth.

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
