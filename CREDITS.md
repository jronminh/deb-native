# Credits

`deb-native` is assembled from other people's work. This file records what it
builds on, and the license each part is under.

## PRoot — the source of dn-monitor's syscall catcher

- <https://github.com/termux/proot> (a fork of
  <https://github.com/proot-me/PRoot>); PRoot was written by Cédric Vincent at
  STMicroelectronics, with the PRoot and Termux contributors.
- License: **GPL-2.0-or-later**. Copyright (C) 2015 STMicroelectronics and the
  PRoot contributors; the notice is kept in the 59 files of `src/tracer/` that
  carry it.
- Used for: `dn-monitor`, the tree's root process and its syscall catcher, is
  built from `src/tracer/`, a reduced fork of `termux/proot` (arm64 only; path
  handling, bindings and extensions reduced). What comes from PRoot is the
  mechanism of catching a process's syscalls with `ptrace` and a `seccomp`
  filter: the launch of a traced child, the filter builder
  (`monitor/monitor.c`), the event loop and its stop state machine
  (`tracee/event.c`), register and memory access (`tracee/reg.c`,
  `tracee/mem.c`), `SIGSYS` handling (`tracee/seccomp.c`), process tracking
  (`tracee/tracee.c`) and the per-syscall handlers (`syscall/`). That core is
  theirs. This project's changes (the catalog-driven gate page, the exec gate,
  fake root, the routing by `dn-policy`) are recorded in the git history of
  `src/tracer/`.
- Going forward: `dn-monitor` is being reduced to catching syscalls and routing
  them by `dn-policy`. PRoot's code stays credited for as long as any of it
  remains in the tree. Code written to replace a part of it uses the same
  kernel interfaces and follows the same technique, described in
  [`docs/reference/ptrace-interception.md`](docs/reference/ptrace-interception.md);
  it is credited as following PRoot's technique even where no source is shared.
- Why a fork and not a fresh tracer: rewriting a syscall's path arguments
  requires `ptrace` (seccomp user-notification can inspect and inject but not
  modify arguments).

## Termux

- <https://github.com/termux/termux-packages> — the Bionic host.
- <https://github.com/termux-pacman/glibc-packages> — the glibc side-install
  (`glibc-runner`, the loader and libraries) whose Android compatibility patch
  series this project's own-glibc patch was forked from.
- Used for: this project's own-glibc patch was a **direct fork of that repo's
  Android compatibility patch series for glibc** (`gpkg/glibc/`,
  GPL-2.0-or-later, same license as glibc itself) — not written from scratch.
  On branch `dn-interp-surgical` that fork is **retired**: the tree takes
  Debian's stock `libc6`/`libc-bin`, and the runtime answers the syscalls
  Android denies ([`patches/README.md`](patches/README.md)). The credit stands
  for the work the fork drew on.
- Without Termux there is no project: the approach is "reuse Termux, fake only
  the Debian layout".

## Debian

- <https://www.debian.org> — every package this project installs is
  Debian's own, unmodified except for the install-time translation
  (`scripts/prefix/dn-translate-deb.sh`) this project adds.
- Used for: `libc6` is Debian's real `glibc` source package
  (`glibc_2.41-12+deb13u4`, including Debian's own ~80-patch
  `debian/patches/series`). On branch `dn-interp-surgical` the tree takes
  Debian's own stock `libc6` `.deb`, unmodified; the earlier Android-patched
  rebuild (the Termux-derived patch on top, repackaged by
  `scripts/glibc/dn-package-glibc.sh`) is retired. `glibc` itself is
  **LGPL-2.1-or-later** (with GPL-licensed pieces Debian's own packaging
  already carries).

## talloc (the Samba Project)

- <https://www.samba.org> (`lib/talloc/` in the `samba` source package) —
  **LGPL-3.0-or-later**, copyright Andrew Tridgell, Jelmer Vernooij and
  the Samba Team.
- Used for: a build-time and run-time dependency of `src/tracer/` (`dn-monitor`),
  inherited from PRoot's own build requirements — PRoot's hierarchical
  memory allocation is built on talloc, unchanged by this project's fork.
  Optional at install; without it, static binaries and programs making their
  own syscalls run untranslated instead of failing to build.

## sudo-less

- <https://github.com/jronminh/sudo-less>
- Used for: the prefix-install approach and its documentation are the starting
  point, and the `apt`/`dpkg` lifecycle-hook idea (`DPkg::Pre-Install-Pkgs` /
  `DPkg::Post-Invoke`) follows it. `sudo-less` solves the same
  problem on a real Debian host with a kernel mount-namespace "view"; this
  project is that idea on Android, where the view is unavailable.

## AI assistance

- Written with AI assistants — **Claude Opus 5.5** and **DeepSeek v4.1 Pro**.
  See the notice at the top of [`README.md`](README.md): the code is
  AI-assisted and unaudited.

## License

`deb-native` is **GPL-3.0-or-later** ([`LICENSE`](LICENSE)). The GPL-2.0-or-later
PRoot code in `src/tracer/` is compatible with it and keeps its own headers.
