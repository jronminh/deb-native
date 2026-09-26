# deb-native

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**Install and run real Debian `arm64` `.deb` packages inside Termux — no root,
no `chroot`, no kernel namespaces.** `apt install PKG` works, and the program
runs by name.

> [!WARNING]
> **Pre-alpha.** Experimental and unaudited; the interface and on-disk layout
> may change without notice, and it can break your Termux setup. Installs
> software outside Termux's own package management. Use a throwaway
> Termux/device until it stabilizes.

> [!CAUTION]
> **AI-assisted and unaudited.** Written with AI assistants; read the code
> before running it (especially `install.sh` and `scripts/`). Not a
> security-reviewed artifact — use at your own risk.

```sh
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/install.sh | sh
exec bash          # or: . ~/.bashrc
figlet hi          # an installed program, run by name
```

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

## How it works

The idea is to **fake only the Debian layout and reuse everything else.** A
`.deb` assumes a real Debian `/`; on Android there is no writable
`/usr /etc /var /opt`, `dpkg` refuses to run unprivileged, and packages are
glibc ELF while Android is Bionic. `deb-native` supplies the missing layout
and reuses the rest.

**The shim.** `native/path-redirect.c` is an `LD_PRELOAD` layer that
interposes the path-taking libc calls (`open`/`openat`/`stat`/`statx`/`exec*`/
`mkdir`/`unlink`/`symlink`/`rename`/…) and rewrites any literal `/usr /etc
/var /opt` to the same path under `$INSTDIR`. To the process, Debian's layout
simply exists; files keep the real app uid and nothing runs as root. (This
stands in for the kernel "view" `sudo-less` uses, which Android forbids:
`unshare(CLONE_NEWUSER)` fails `EINVAL` here — see
[`docs/findings.md`](docs/findings.md).)

**Maintainer scripts run inside the fake Debian.** A `postinst`'s shebang is
resolved by the kernel, which follows only one `#!` level; `native/dn-launch.c`
is a tiny Bionic ELF that sets up the shim and execs Termux's glibc
`bash`/`perl`, so a script runs in the prefix — including `preinst`, before
`dpkg` has unpacked anything.

**Real prefix, real `dpkg`.** Packages unpack with Termux's own `dpkg`
(`--instdir`/`--admindir`/`--force-script-chrootless`) into `$INSTDIR/root`,
driven by a prefix-scoped `apt.conf` pointed at the Debian `arm64` repository.

**Real glibc, reused.** Termux's `$PREFIX/glibc` side-install is repointed at
with `grun --configure` (a one-time ELF `PT_INTERP` edit), so Debian glibc
binaries run unmodified. Debian dependencies Termux already provides are
seeded as installed rather than duplicated (`scripts/native-seed.sh`).

**Hooked into `apt`/`dpkg`.** `DPkg::Pre-Install-Pkgs` patches each `.deb`
before unpack (so even `preinst` sees the fake Debian); `DPkg::Post-Invoke`
repoints new ELFs at Termux's glibc and regenerates launchers. A `dpkg`
wrapper does the same for `dpkg -i`. The user types `apt install PKG`; nothing
else.

**Run by name.** Each installed program gets a launcher under
`$INSTDIR/usr/lib/deb-native/bin`, put first on `PATH` — ELF targets get the
shim, scripts go through the glibc shell. Termux's own `termux-exec` is never
disabled.

**Beyond libc.** Static binaries, inline `svc`, and libc-internal reads (NSS)
bypass the shim; a syscall-level tracer (`tracer/`, a reduced proot) handles
them — see [`docs/syscall-boundary.md`](docs/syscall-boundary.md).

## Routing: Termux wins

Termux's own `apt` and `dpkg` are wrapped and route per package — no new
command, no new name. A package Termux also provides installs normally
(aarch64, Bionic, untouched); only a name that exists in the Debian `arm64`
repo and *not* in Termux goes through the prefix:

```sh
apt install bsdmainutils   # Debian-only -> the prefix
apt install cowsay         # Termux has it -> normal Termux install
dpkg -i ./pkg_arm64.deb    # arm64 -> the prefix; aarch64 -> Termux
```

`install.sh` is idempotent. Pick a prefix / packages from a checkout:
`sh install.sh ~/.dn figlet tree`.

## Scope

Install (reach `dpkg` status `ii`) and run by name, unprivileged — not a
faithful Debian, not isolation. Coverage is a *named boundary*, not a promise:
[`docs/standard.md`](docs/standard.md) (which packages),
[`docs/shim-coverage.md`](docs/shim-coverage.md) (measured libc coverage),
[`docs/syscall-boundary.md`](docs/syscall-boundary.md) (the rest). No fake
root and no isolation; no services yet (`runit` is the candidate); not for
packages that need root (system users, `setuid`, TUN, kernel modules).

## Status

**Pre-alpha.** The base install and run path are solid; run/integrate/service
and the syscall tracer are still being built. `termux-dn-doctor` checks the
common breakages (a leaked `APT_CONFIG`, a clobbered Termux `sources.list`,
stale wrappers) and `--fix`es them.

## Requirements

Termux with `git`, `clang`, and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`, `coreutils-glibc`,
`bash-glibc`, `perl`, the loader and libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) ships with Termux.

## Documentation

- [`docs/design.md`](docs/design.md) — the design end to end.
- [`docs/standard.md`](docs/standard.md) — package scope.
- [`docs/shim-coverage.md`](docs/shim-coverage.md) — measured shim coverage.
- [`docs/syscall-boundary.md`](docs/syscall-boundary.md) — beyond libc.
- [`docs/direct-usage.md`](docs/direct-usage.md) — tracer investigation + fork-lite plan.
- [`docs/findings.md`](docs/findings.md) — engineering log.
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
