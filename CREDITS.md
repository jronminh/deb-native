# Credits

`deb-native` builds on other people's work. This file records what it uses, what
it follows, and the license of each part.

## PRoot — the technique of the tracer

- <https://github.com/proot-me/PRoot> and its Termux fork
  <https://github.com/termux/proot>; PRoot was written by Cédric Vincent at
  STMicroelectronics, with the PRoot and Termux contributors (GPL-2.0-or-later).
- Followed, not copied: `dn-tiny` (`src/dn-tiny/`) is written from scratch and
  shares no source with PRoot, but it uses the technique PRoot established for
  running a foreign userland without root: catch a process's syscalls with
  `ptrace` behind a `seccomp` filter that lets everything else run, rewrite their
  path arguments into the tree, and copy a file where the platform refuses a hard
  link. Up to the 0.7 series this repository carried a reduced fork of PRoot as
  its tracer (`dn-monitor`, tag `legacy-dn-monitor`).

## Termux

- <https://github.com/termux/termux-packages> — the host the prefix is built in
  and run from, its compilers, and its `debootstrap` package, whose first stage
  `scripts/build-prefix.sh` uses to fetch and unpack the base packages.
- Without Termux there is no project.

## Debian

- <https://www.debian.org> — every package in a prefix is Debian's own,
  unmodified, installed by Debian's own `debootstrap` second stage, `dpkg` and
  `apt`; `libc6` is Debian's stock glibc (LGPL-2.1-or-later).

## Syscall interception research

- gVisor's `systrap` platform (<https://gvisor.dev>): busy-polling for the next
  event rather than sleeping, as `dn-tiny -s` does.
- `zpoline` (Yasukata et al., USENIX ATC 2023), `svc-hook` (Moroo, Tazaki,
  Yasukata, Middleware 2025) and ASC-Hook (Shen et al., LCTES 2025): rewriting
  a process's own syscall instructions into branches to a hook, as the shim does
  for libc's `svc` sites (`patch_libc` in `src/dn-tiny/lane.c`). The code is this
  project's own.

## sudo-less

- <https://github.com/jronminh/sudo-less>
- The prefix-install idea started there: `sudo-less` solves the same problem on a
  real Debian host with a mount-namespace view; this project is that idea on
  Android, where the view is unavailable.

## AI assistance

- Written with AI assistants — **Claude Opus 5.5** and **DeepSeek v4.1 Pro**. The
  code is AI-assisted and not independently audited (see
  [`SECURITY.md`](SECURITY.md)).

## License

`deb-native` is **GPL-3.0-or-later** ([`LICENSE`](LICENSE)).
