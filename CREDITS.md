# Credits

`deb-native` is assembled from other people's work. This file records what it
builds on, and the license each part is under.

## PRoot — the syscall tracer

- <https://github.com/termux/proot> (a fork of
  <https://github.com/proot-me/PRoot>)
- License: **GPL-2.0-or-later**. Copyright STMicroelectronics and the PRoot
  contributors.
- Used for: `tracer/` is a reduced fork of `termux/proot`
  (arm64-only, path handling, extensions removed). The original copyright and
  license headers are kept in every derived file; see
  [`tracer/README.md`](tracer/README.md) for the pinned commit and the exact
  set of changes. The `ptrace` syscall-interception core — the hard part — is
  theirs.
- Why a fork and not a fresh tracer: rewriting a syscall's path arguments
  requires `ptrace` (seccomp user-notification can inspect and inject but not
  modify arguments). See [`docs/direct-usage.md`](docs/direct-usage.md).

## Termux

- <https://github.com/termux/termux-packages> — the Bionic host, and the
  non-root `apt`/`dpkg` patches the project reuses **as-is**.
- <https://github.com/termux-pacman/glibc-packages> — the glibc side-install
  (`glibc-runner`/`grun`, `coreutils-glibc`, `bash-glibc`, `perl`, the loader
  and libraries) that every Debian glibc binary here is repointed at.
- Without Termux there is no project: the approach is "reuse Termux, fake only
  the Debian layout".

## sudo-less

- <https://github.com/jronminh/sudo-less>
- Used for: the prefix-install approach and its documentation are the starting
  point, and the `apt`/`dpkg` lifecycle-hook idea (`DPkg::Pre-Install-Pkgs` /
  `DPkg::Post-Invoke`) follows it. See
  [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md). `sudo-less` solves the same
  problem on a real Debian host with a kernel mount-namespace "view"; this
  project is that idea on Android, where the view is unavailable.

## AI assistance

- Written with AI assistants — **Claude Opus 5.5** and **DeepSeek v4.1 Pro**.
  See the notice at the top of [`README.md`](README.md): the code is
  AI-assisted and unaudited.

## License

`deb-native` is **GPL-3.0-or-later** ([`LICENSE`](LICENSE)). The GPL-2.0-or-later
PRoot code in `tracer/` is compatible with it and keeps its own headers.
