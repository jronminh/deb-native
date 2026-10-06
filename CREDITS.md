# Credits

`deb-native` is assembled from other people's work. This file records what it
builds on, and the license each part is under.

## PRoot — the syscall tracer

- <https://github.com/termux/proot> (a fork of
  <https://github.com/proot-me/PRoot>)
- License: **GPL-2.0-or-later**. Copyright STMicroelectronics and the PRoot
  contributors.
- Used for: `src/tracer/` is a reduced fork of `termux/proot`
  (arm64-only, path handling, extensions removed). The original copyright and
  license headers are kept in every derived file. The `ptrace`
  syscall-interception core — the hard part — is theirs.
- Why a fork and not a fresh tracer: rewriting a syscall's path arguments
  requires `ptrace` (seccomp user-notification can inspect and inject but not
  modify arguments).

## Termux

- <https://github.com/termux/termux-packages> — the Bionic host.
- <https://github.com/termux-pacman/glibc-packages> — the glibc side-install
  (`glibc-runner`, the loader and libraries) that built this project's own
  glibc, and the fallback before it is wired in.
- Used for: this project's own-glibc patch,
  [`patches/dn-glibc-android.patch`](patches/dn-glibc-android.patch),
  is a **direct fork of this repo's own Android compatibility patch series
  for glibc** (`gpkg/glibc/`, GPL-2.0-or-later, same license as glibc
  itself) — not written from scratch. The patch carries every file the
  fork needed, retargeted from Termux's dual-prefix layout to this
  project's single fixed prefix. See
  [`patches/README.md`](patches/README.md)
  for the exact provenance and what was changed vs. kept as-is.
- Without Termux there is no project: the approach is "reuse Termux, fake only
  the Debian layout".

## Debian

- <https://www.debian.org> — every package this project installs is
  Debian's own, unmodified except for the install-time translation
  (`scripts/prefix/dn-translate-deb.sh`) this project adds.
- Used for: `libc6` is Debian's real `glibc` source package
  (`glibc_2.41-12+deb13u4`, including Debian's own ~80-patch
  `debian/patches/series`), with the Termux-derived Android patch above
  applied on top and repackaged as a `.deb`
  (`scripts/glibc/dn-package-glibc.sh`) — Debian's own maintainer
  scripts/triggers/symbols/doc are reused as-is, only the payload is
  this project's build. `glibc` itself is
  **LGPL-2.1-or-later** (with GPL-licensed pieces Debian's own packaging
  already carries).

## talloc (the Samba Project)

- <https://www.samba.org> (`lib/talloc/` in the `samba` source package) —
  **LGPL-3.0-or-later**, copyright Andrew Tridgell, Jelmer Vernooij and
  the Samba Team.
- Used for: a build-time and run-time dependency of `src/tracer/` (`dn-trace`),
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
