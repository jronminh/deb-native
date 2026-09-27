# deb-native

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**Install and run real Debian `arm64` `.deb` packages inside Termux — no root,
no `chroot`, no kernel namespaces.** `apt install PKG` works, and the program
runs by name.

> [!WARNING]
> **Pre-alpha, AI-assisted, not security-reviewed.** Tested: a fresh install
> on vanilla Termux, and a 100-package survey of Debian 13 "trixie"
> ([`docs/survey-0.2.0.md`](docs/survey-0.2.0.md): 99 install, 98 run within
> the survey's limits, the other 2 fixed since). It never touches Termux's
> own `sources.list`, `dpkg` database or binaries. Still: written with AI
> assistants and not independently audited, so read `install.sh`/`scripts/`
> before running them; the layout can change between releases; heavy
> packages (toolchains) are not there yet. Use a throwaway Termux/device.

```sh
# pinned pre-alpha release:
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/v0.2.0-prealpha/install.sh | DEB_NATIVE_REF=v0.2.0-prealpha sh

# then restart Termux (or: . ~/.bashrc)
apt install figlet # the prefix's apt: Debian's packages
figlet hi          # an installed program, run by name
```

Or the rolling edge: replace both `v0.2.0-prealpha` occurrences with `main`.

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

## Why this exists

The usual way to get Debian on Android is a **separate rootfs image under
proot** (or root): every syscall of every program through proot.
`deb-native` instead keeps a small Debian root of its own, `~/.dn`, and
runs its programs as ordinary Termux processes:

- **Its own apt and dpkg, Termux's binaries.** The prefix has its own dpkg
  database, sources and Debian base; the `apt`/`dpkg` that manage it are
  Termux's own, through launchers. Nothing is rebuilt.
- **Only glibc comes from Termux.** The prefix's `libc6` is a stand-in for
  Termux's patched glibc (the one piece Android needs); everything else is
  Debian's own package.
- **No proot for the common case.** Programs start through a tiny loader
  (`ld-dn`) that sets up an in-process path shim; only static binaries,
  raw syscalls and NSS need the tracer.

Not an emulator and not isolation: **install and run, not emulate**.

## How it works

**The prefix.** `install.sh` builds `~/.dn` debootstrap-style: Debian's
index and archive keyring (checked against pinned fingerprints), the
stand-ins (`libc6` -> Termux's glibc, `dpkg`/`apt` -> Termux's binaries),
then the Debian base (`base-files`, `base-passwd`, `dash`, `coreutils`,
`debconf`, `ca-certificates`, ...) installed by the prefix's own dpkg and
held. Termux's home is the prefix's `/root`.

**Translation at install.** An apt hook translates every `.deb` before dpkg
sees it (`scripts/dn-translate-deb.sh`): `Architecture: all` -> `arm64`,
programs' ELF interpreter -> `ld-dn` and library path -> the prefix,
script `#!` lines and maintainer scripts -> the prefix's shell, hard links
-> copies (Android forbids them), per-package fixes in `custom/`.

**The runtime.** `native/ld-dn.c` is every Debian program's interpreter:
it finds the prefix, puts the path shim (`native/path-redirect.c`, an
`LD_PRELOAD` layer rewriting `/usr /etc /var /opt /root` into the prefix)
in the environment, and hands over to glibc's loader — however the
program is started, from Termux's side too. Maintainer scripts run under
`dn-shell`, with a privilege layer first on their `PATH`
(`usr/lib/deb-native/priv/`: no-op `chown`, `update-rc.d`, ...; prefix-aware
`update-alternatives`, `dpkg-divert`, `getent`).

**Beyond libc.** Static binaries, programs making their own syscalls and
glibc's NSS bypass the shim; `dn-trace` (`tracer/`, a trimmed PRoot, built
at install when `make` and `libtalloc` are present) rewrites their paths at
the syscall level — see [`docs/tracer-0.2.0.md`](docs/tracer-0.2.0.md).

**Run by name.** Installed programs are linked into
`~/.dn/usr/lib/deb-native/bin`, first on `PATH`; the base system's tools
are not, so Termux's own `ls`, `awk`, ... stay in charge.

## apt and dpkg

In your shell, `apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg` and
`dpkg-query` are **the prefix's** (Debian's packages), through a managed
block in `~/.bashrc`. Termux's are `pkg` (as Termux recommends),
`termux-apt` and `termux-dpkg`; scripts are unaffected, since aliases never
reach them. To undo: `sed -i '/# deb-native/d' ~/.bashrc` and delete
`~/.dn`.

`install.sh` is idempotent. From a checkout: `sh install.sh [PREFIX] [pkg ...]`.
The full install log is in `~/.dn/var/log/`.

## Scope

The same packages as [`sudo-less`](https://github.com/jronminh/sudo-less),
by Debian section ([`docs/standard.md`](docs/standard.md)): install (reach
`ii`) and run by name, unprivileged. Not yet: **services** (a package that
ships one installs, the service does not run; `runit` is the plan), and
packages that need root (system users, `setuid`, TUN, kernel modules;
`sudo` modes are planned). Toolchains wait for `libc6`'s Debian identity
([`TODO.md`](TODO.md)).

## Status

**Pre-alpha, 0.2.0.** Install and run are measured, not just hand-checked
([`docs/survey-0.2.0.md`](docs/survey-0.2.0.md)). `termux-dn-doctor` checks
the common breakages. Next: `libc6` as Debian's exact identity
(toolchains), then services, then `sudo`.

### Experimental: true fusion (separate branch, not for general use)

The [`naibed`](https://github.com/jronminh/deb-native/tree/naibed)
branch builds on this project's core to go one step further: it
transforms Termux's own `$PREFIX` into a Debian `arm64` system -- Debian
as apt's only source, packages installed straight into Termux's prefix
and dpkg database, Termux reduced to the packages it runs on. It is
**one-way and far less safe than `main`**: a bad package can break Termux
itself, not just a Debian program, and there is no switch back. See its
[`docs/true-fusion.md`](https://github.com/jronminh/deb-native/blob/naibed/docs/true-fusion.md)
before touching it. `main`'s separate prefix stays the recommended path.

## Requirements

Termux with `git`, `clang`, `patchelf`, and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`, `coreutils-glibc`,
`bash-glibc`, `perl`, the loader and libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) ships with Termux.

Optional: `make` and `libtalloc` (`pkg install make libtalloc`) to build the
syscall tracer, needed by static programs and ones making their own syscalls.
Without them those programs run untranslated.

## Documentation

- [`docs/design.md`](docs/design.md) — the design end to end.
- [`docs/standard.md`](docs/standard.md) — package scope.
- [`docs/shim-coverage.md`](docs/shim-coverage.md) — measured shim coverage.
- [`docs/syscall-boundary.md`](docs/syscall-boundary.md) — beyond libc.
- [`docs/direct-usage.md`](docs/direct-usage.md) — tracer investigation + fork-lite plan.
- [`docs/tracer-0.2.0.md`](docs/tracer-0.2.0.md) — the tracer (`dn-trace`) in 0.2.0: role, changes, tests, measurements.
- [`docs/survey-0.2.0.md`](docs/survey-0.2.0.md) — 100 Debian packages installed and run in the 0.2.0 prefix.
- [`docs/design-0.2.0.md`](docs/design-0.2.0.md) — the 0.2.0 self-contained prefix (partly superseded by `TODO.md`'s decisions).
- [`docs/runtime-failures.md`](docs/runtime-failures.md) — what breaks when *running* a program.
- [`docs/tailscale.md`](docs/tailscale.md) — the static-daemon goal (userspace networking).
- [`docs/findings.md`](docs/findings.md) — engineering log.
- [`docs/multiarch-mechanics.md`](docs/multiarch-mechanics.md) — dpkg multi-arch mechanics, shared with the true fusion branch (`naibed`).
- [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) — method, side by side with `sudo-less`.
- [`tracer/README.md`](tracer/README.md) — the reduced proot (`fork-lite`).
- [`TODO.md`](TODO.md) — roadmap · [`AGENTS.md`](AGENTS.md) — conventions.

## Credit & license

Built on other people's work — see [`CREDITS.md`](CREDITS.md):

- **[PRoot](https://github.com/termux/proot)** (`proot-me/PRoot`,
  GPL-2.0-or-later) — the `ptrace` syscall-interception core; `tracer/` is a
  reduced fork with its headers kept.
- **[Termux](https://github.com/termux/termux-packages)** and
  [`glibc-packages`](https://github.com/termux-pacman/glibc-packages) — the
  host, the non-root `apt`/`dpkg` patches, and the glibc userland.
- **[sudo-less](https://github.com/jronminh/sudo-less)** — the prefix-install
  approach and the `apt`/`dpkg` lifecycle-hook idea.

Written with AI assistance (**Claude Opus 5.5**, **DeepSeek v4.1 Pro**).
GPL-3.0-or-later — see [`LICENSE`](LICENSE).
