# deb-native

**Termux, made into a Debian you can install into.** Real `arm64` Debian
`.deb` packages — installed with `apt`/`dpkg`, run by name — inside Termux,
with **no root, no kernel namespaces, and no proot/container**.

The trick is not emulation, not a chroot, and not fake root. `deb-native`
fuses two things Termux already has — its own `apt`/`dpkg` and its glibc
side-install — with a small **shim we build ourselves** that fakes the one
thing Debian assumes and Android lacks: a writable `/usr`, `/etc`, `/var`,
`/opt`.

> [!CAUTION]
> **AI-assisted and unaudited.** The scripts, shim and docs were written with
> AI assistants. It still has bugs, and it installs software outside Termux's
> own package management. Read the code before you run it — especially
> `install.sh` and the scripts under `scripts/`. This is not a
> security-reviewed artifact; use it at your own risk.

```sh
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/install.sh | sh
```

That fetches the project into `~/.deb-native` and sets up the prefix. Then
open a new shell (`exec bash`). Termux's `apt` now routes per package:
anything Termux also provides installs normally, a Debian-only name goes to
the prefix, and installed programs run by name:

```sh
apt install bsdmainutils   # Debian-only -> deb.debian.org -> the prefix
figlet hi                  # installed program, run by name
```

Installing `lua5.4` — a package Termux does not carry — and running it. No
root, no proot, no chroot:

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

Status: **working prototype** (2026-09-26). A fresh prefix bootstraps the
full Debian base set (28 packages, all `Status: install ok installed`) and
installs leaf packages on top of it. See [Status](#status) for what works
and [Roadmap](TODO.md) for what is next.

---

## The method: we fake Debian, not root

A `.deb` assumes `/` is a real Debian system. On stock Android that is false
in three ways at once: there is no writable `/usr /etc /var /opt`, `dpkg`
will not run as an unprivileged app, and the package's binaries are glibc
ELF, not Bionic. `deb-native` fakes the one thing that is missing — the
Debian layout — and reuses everything else Termux already has:

1. **We fake Debian, not root.** No uid-0 illusion (`fakeroot`, `proot -0`),
   no ptrace, no mount. A **shim we build ourselves** — a custom
   libc-interposition layer (`native/path-redirect.c`) — sits in front of the
   path calls a program makes (`open`/`openat`/`stat`/`statx`/`exec*`/`mkdir`/
   `unlink`/`symlink`/`rename`/…) and rewrites any literal
   `/usr`, `/etc`, `/var`, `/opt` to the same path under `$INSTDIR`. To the
   process, Debian's layout simply exists; the files keep the real app uid
   and nothing runs as root. (This stands in for the kernel "view" sudo-less
   uses, which is unavailable here: `unshare(CLONE_NEWUSER)` fails `EINVAL`
   and FUSE is closed.)
2. **Real Debian files, in a prefix.** The packages are unpacked with
   Termux's own `dpkg` (which already ships the non-root patches) into a
   separate prefix, `$INSTDIR/root`, using dpkg's stock
   `--instdir` / `--admindir` / `--force-script-chrootless` flags and a
   prefix-scoped `apt.conf`. No fork of apt/dpkg is needed.
3. **Real glibc.** Termux ships a full glibc userland
   (`termux-pacman/glibc-packages`) — `bash`, `coreutils`, `perl`, the
   loader, the libraries. Debian binaries are repointed at it with
   `grun --configure` (a one-time ELF `PT_INTERP` + `RUNPATH` edit), so a
   Debian glibc `.deb` runs unmodified.
4. **A real interpreter for maintainer scripts.** A package's `postinst`
   is executed by the *kernel* resolving its shebang, and the kernel
   follows only one `#!` level. `native/dn-launch.c` is a tiny **Bionic
   ELF** that sets up the shim and execs Termux's glibc `bash` (or `perl`),
   so maintainer scripts run **inside** the fake Debian — including
   `preinst`, before `dpkg` has even unpacked the package.
5. **Hang the pipeline on apt/dpkg, the way `sudo-less` does.** A
   prefix-scoped apt config points Termux's own `apt` at the Debian `arm64`
   repository and the prefix, with `DPkg::Pre-Install-Pkgs` patching each
   `.deb` before dpkg unpacks it (so even `preinst` sees the fake Debian)
   and `DPkg::Post-Invoke` repointing new ELFs at Termux's glibc and
   regenerating launchers. A `dpkg` wrapper does the same for direct
   `dpkg -i`. A user types `apt install PKG`; nothing else.
6. **Launch programs by name.** After each install, a wrapper is generated
   per program under `$INSTDIR/usr/lib/deb-native/bin` (ELF targets get the
   shim; scripts go through the glibc shell), and that directory is put
   first on `PATH`. `figlet hi` just works.
7. **Termux stays intact.** Termux's own shim (`termux-exec`) is never
   disabled; when a program running under our shim forks a *Bionic* child,
   the shim hands `termux-exec` back to it. The two userlands coexist.

Everything above is a shell script, our own C shim, and one tiny C launcher
— no patched apt/dpkg, no helpers to install, no kernel features.

## How it relates to `sudo-less`

[`sudo-less`](https://github.com/jronminh/sudo-less) does the same job on a
**real Debian host** (non-root user, `~/.local`) using a private mount
namespace + unprivileged overlayfs ("the view") and `systemd --user`.
`deb-native` is that idea ported to Android, where the view and systemd are
unavailable, so the mechanism is inverted:

| | sudo-less (Debian host) | deb-native (Termux/Android) |
|---|---|---|
| base | Debian-on-Debian | Termux (Bionic) **fused** with our own libc-interposition shim |
| virtualization | kernel view: user+mount namespace + overlayfs | our own shim: a custom libc interposer (`native/path-redirect.c`) rewrites `/usr /etc /var /opt` → `$INSTDIR` |
| shell / runtime | the host's own | Termux's pre-existing glibc `bash`/`perl`/coreutils |
| apt / dpkg | a fork retargeted *back* to Debian | Termux's own, reused **as-is** |
| dependency reuse | seed from the host's dpkg db | seed from Termux's installed `*-glibc` packages |

Full side-by-side: [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md).

## Quick start

Requirements: Termux with `git`, `clang`, and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`/`grun`, `coreutils-glibc`,
`bash-glibc`, `perl`, and the loader/libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) is already in Termux.

```sh
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/install.sh | sh
exec bash                                       # or: . ~/.bashrc
figlet hi                                       # installed program, by name
```

From a checkout you can pick the prefix and pre-install packages in one go:

```sh
sh install.sh ~/.dn figlet tree
```

After activation, **Termux's own `apt` and `dpkg` route per package** — no
wrapper command, no new name. The rule is **Termux wins**: a package Termux
also provides installs normally (aarch64, Bionic, untouched); only a name
that exists in the Debian `arm64` repo and *not* in Termux goes through the
prefix and the pipeline (the `sudo-less` hook approach):

```sh
apt install bsdmainutils  # Debian-only -> deb.debian.org -> the prefix
apt install cowsay        # Termux has it -> normal Termux install
dpkg -i ./pkg_arm64.deb   # arm64 .deb -> the prefix; aarch64 -> Termux
```

`install.sh` is idempotent: run it again with more packages, or point it at
a different prefix. Under the hood:

- `scripts/setup-apt-prefix.sh` — point `apt` at a Debian `arm64` repo,
  scope it to the prefix, bootstrap the base set, generate launchers.
- `scripts/apt-install.sh` — resolve + download with `apt`, patch each
  `.deb`, drive `dpkg --unpack` / `--configure` one package at a time.
- `scripts/make-apt-wrappers.sh` — arch-aware `apt`/`apt-get`/`apt-cache`/
  `dpkg` wrappers (the dispatcher; Termux wins).
- `scripts/make-launchers.sh` + `scripts/dn-activate.sh` — program wrappers
  + `PATH`.

## What works today

- Fresh bootstrap of the Debian base: `base-files`, `base-passwd`, `dash`,
  `debianutils`, `debconf`, `cdebconf`, `openssl`, `ca-certificates`,
  `mawk` and their libraries — 28 packages to `ii`.
- Installing real leaf packages and their dependency chains from
  `deb.debian.org`, with Debian dependencies that Termux's glibc install
  already provides reused in place (no duplicate files).
- Programs that read hardcoded absolute paths (`/usr/share/figlet`, …) run
  correctly once launched through the shim.
- Programs run by name from a new shell.

## What it does not do

- **No fake root — we fake Debian instead.** No uid-0 illusion
  (`fakeroot`/`proot -0`); files keep the real unprivileged uid, and `dpkg`
  runs `--force-not-root`.
- **No isolation.** The kernel here cannot build a user namespace, so an
  install does not hide `$HOME` or provide an empty `/run` the way
  sudo-less's view does. Run only packages you trust.
- **No system services yet.** No `systemd --user` on Termux; `runit`
  (`termux-services`) is the candidate ([research](docs/design.md)).
- **Not for packages that need root** — system users/groups, `setuid`,
  firewall/TUN, kernel modules.
- Two known nits: `update-alternatives` writes its links into Termux's own
  prefix, and a few maintainer scripts that fork Bionic `sed`/`find` can't
  see prefix paths. Neither blocks a package reaching `ii`.

## Status

The base environment and install path are solid; the run/integrate/service
half is still being built. Coverage is measured the way `sudo-less` measures
its own — a random per-section sample (see `docs/survey*.md`); the last
number predates the base-env fix and is being re-measured. The full,
chronological engineering log — including every dead end and the syscall
evidence — is in [`docs/`](docs/).

Next steps are tracked in [`TODO.md`](TODO.md).

## Documentation

- [`docs/design.md`](docs/design.md) — how it works end to end: faking the
  Debian layout, native dependency reuse, the install path, run-time
  wrappers, the apt/dpkg hooks, services, and prior art.
- [`docs/findings.md`](docs/findings.md) — the engineering log (every dead
  end, and the syscall evidence behind the decisions).
- [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) — the method, side by side
  with `sudo-less`.

## Prior art / credit

- [`sudo-less`](https://github.com/jronminh/sudo-less) — the prefix-install
  approach and its documentation are the starting point; the `apt`/`dpkg`
  patches both projects build on originate in Termux.
- Termux and `termux-pacman/glibc-packages` — the Bionic host and the glibc
  userland the fusion relies on.
- Built with AI assistance — **Claude Opus 5.5** and **DeepSeek v4.1 Pro**.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
