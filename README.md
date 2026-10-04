# deb-native

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**Real Debian `arm64` `.deb` packages inside Termux: no root, no `chroot`,
no namespaces.** `apt install PKG` works and the program runs by name; a
small Debian tree in `~/.dn`, Termux left untouched.

> [!WARNING]
> **Pre-alpha, AI-assisted, not independently audited.** Read `install.sh`
> and `scripts/` before running them. The on-disk layout can change between
> releases, heavy packages (toolchains) aren't fully supported, and this has
> had no security review. Use a throwaway Termux install or device.

## Requirements

Termux with `git`, `clang`, `patchelf`, and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`, `coreutils-glibc`,
`bash-glibc`, `perl`, the loader and libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) ships with Termux.

Optional: `make` and `libtalloc` (`pkg install make libtalloc`) to build the
syscall tracer, needed by static programs and ones making their own syscalls.
Without them those programs run untranslated.

## Install

```sh
# pinned pre-alpha release:
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/v0.6.0+s.1-prealpha/install.sh | DEB_NATIVE_REF=v0.6.0+s.1-prealpha sh

# then restart Termux — the new session is the Debian userland
apt install figlet # the prefix's apt: Debian's packages
figlet hi          # an installed program, run by name
```

Rolling edge: replace both `v0.6.0+s.1-prealpha` occurrences with `main`.
`install.sh` is idempotent — from a checkout: `sh install.sh [PREFIX] [pkg ...]`;
log in `~/.dn/var/log/`.

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

## Documentation

Full map in [`docs/README.md`](docs/README.md). Start with
[`docs/spec/status.md`](docs/spec/status.md) for where it is today and
[`docs/spec/design.md`](docs/spec/design.md) for how it works.

- [`docs/spec/`](docs/spec/README.md) — the design, install flow, host/userland.
- [`docs/reference/`](docs/reference/README.md) — look-up facts and catalogs
  (Android gates, shim/tracer coverage, known issues).
- [`docs/notes/`](docs/notes/README.md) — comparisons and prior art
  ([`alternatives.md`](docs/notes/alternatives.md) is the full one).
- [`docs/log/`](docs/log/README.md) — engineering history ·
  [`docs/guides/`](docs/guides/README.md) — how-tos.
- [`tracer/README.md`](tracer/README.md) — `dn-trace` ·
  [`AGENTS.md`](AGENTS.md) — conventions.

## Credit & license

Built on other people's work — see [`CREDITS.md`](CREDITS.md):

- **[PRoot](https://github.com/termux/proot)** (`proot-me/PRoot`,
  GPL-2.0-or-later) — the `ptrace` syscall-interception core; `tracer/` is a
  reduced fork with its headers kept.
- **[talloc](https://www.samba.org)** (the Samba Project, LGPL-3.0-or-later)
  — PRoot's (and so `tracer/`'s) memory allocator; a build/run dependency,
  unchanged.
- **[Termux](https://github.com/termux/termux-packages)** — the Bionic host
  and the non-root `apt`/`dpkg` patches this project reuses as-is.
- **[`glibc-packages`](https://github.com/termux-pacman/glibc-packages)**
  (`termux-pacman`) — the glibc side-install every Debian glibc binary was
  repointed at before 0.5.0 (still the `install.sh` default); more directly,
  0.5.0's own-glibc patch
  ([`third_party/glibc-android-patches/`](third_party/glibc-android-patches/))
  is a **fork of this repo's own Android compatibility patches for glibc**
  itself, not written from scratch.
- **[Debian](https://www.debian.org)** — every installed package is
  Debian's own, unmodified beyond install-time translation; 0.5.0's `libc6`
  is Debian's real `glibc` source package repackaged with the patch above.
- **[sudo-less](https://github.com/jronminh/sudo-less)** — the prefix-install
  approach and the `apt`/`dpkg` lifecycle-hook idea.

Written with AI assistance (**Claude Opus 5.5**, **DeepSeek v4.1 Pro**).
GPL-3.0-or-later — see [`LICENSE`](LICENSE).
