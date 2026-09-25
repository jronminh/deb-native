# deb-native

**Termux, made into a Debian you can install into.** Real `arm64` Debian
`.deb` packages — installed with `apt`/`dpkg`, run by name — inside Termux,
with **no root, no kernel namespaces, and no proot/container**.

The trick is not emulation and not a chroot. `deb-native` fuses two things
Termux already has — its own `apt`/`dpkg` and its glibc side-install — with
a small userspace **native overlay** that fakes the one thing Debian assumes
and Android lacks: a writable `/usr`, `/etc`, `/var`, `/opt`.

```sh
git clone https://github.com/jronminh/deb-native
cd deb-native
./install.sh ~/.dn figlet tree
exec bash
figlet hi
```

Status: **working prototype** (2026-09-25). A fresh prefix bootstraps the
full Debian base set (28 packages, all `Status: install ok installed`) and
installs leaf packages on top of it. See [Status](#status) for what works
and [Roadmap](TODO.md) for what is next.

---

## The method: how Termux becomes "Debian"

A Debian package assumes `/` is a real Debian system. On stock Android that
is false in three ways at once: there is no writable `/usr /etc /var /opt`,
`dpkg` will not run as an unprivileged app, and the package's binaries are
glibc ELF, not Bionic. `deb-native` addresses each:

1. **Real Debian files, in a prefix.** The packages are unpacked with
   Termux's own `dpkg` (which already ships the non-root patches) into a
   separate prefix, `$INSTDIR/root`, using dpkg's stock
   `--instdir` / `--admindir` / `--force-script-chrootless` flags and a
   prefix-scoped `apt.conf`. No fork of apt/dpkg is needed.
2. **Real glibc.** Termux ships a full glibc userland
   (`termux-pacman/glibc-packages`) — `bash`, `coreutils`, `perl`, the
   loader, the libraries. Debian binaries are repointed at it with
   `grun --configure` (a one-time ELF `PT_INTERP` + `RUNPATH` edit), so a
   Debian glibc `.deb` runs unmodified.
3. **Fake the absolute-path view — this is the native overlay.** A small
   `LD_PRELOAD` library (`native/path-redirect.c`) intercepts the libc
   calls that touch paths — `open`/`openat`/`stat`/`statx`/`exec*`/`mkdir`/
   `unlink`/`symlink`/`rename`/… — and rewrites any literal
   `/usr`, `/etc`, `/var`, `/opt` path to the same path under `$INSTDIR`.
   No mount, no namespace, no kernel privilege: just userspace symbol
   interposition. (Not available on this device: `unshare(CLONE_NEWUSER)`
   fails `EINVAL`, FUSE is closed — so the kernel "view" sudo-less uses is
   replaced by this.)
4. **A real interpreter for maintainer scripts.** A package's `postinst`
   is executed by the *kernel* resolving its shebang, and the kernel
   follows only one `#!` level. `native/dn-launch.c` is a tiny **Bionic
   ELF** that sets the shim environment and execs Termux's glibc `bash`
   (or `perl`), so maintainer scripts run **inside** the fake Debian with
   the overlay active — including `preinst`, before `dpkg` has even
   unpacked the package.
5. **Hang the pipeline on apt/dpkg, the way `sudo-less` does.** A
   prefix-scoped apt config points Termux's own `apt` at the Debian `arm64`
   repository and the prefix, with `DPkg::Pre-Install-Pkgs` patching each
   `.deb` before dpkg unpacks it (so even `preinst` sees the overlay) and
   `DPkg::Post-Invoke` repointing new ELFs at Termux's glibc and regenerating
   launchers. A `dpkg` wrapper does the same for direct `dpkg -i`. A user
   types `apt install PKG`; nothing else.
6. **launch programs by name.** After each install, a wrapper is generated
   per program under `$INSTDIR/usr/lib/deb-native/bin` (ELF targets get the
   overlay; scripts go through the glibc shell), and that directory is put
   first on `PATH`. `figlet hi` just works.
7. **Termux stays intact.** Termux's own `LD_PRELOAD` (`termux-exec`) is
   never disabled; when a program running under the overlay forks a
   *Bionic* child, the shim hands `termux-exec` back to it. The two
   userlands coexist.

Everything above is a shell script, one C shim, and one tiny C launcher —
no patched apt/dpkg, no helpers to install, no kernel features.

## How it relates to `sudo-less`

[`sudo-less`](https://github.com/jronminh/sudo-less) does the same job on a
**real Debian host** (non-root user, `~/.local`) using a private mount
namespace + unprivileged overlayfs ("the view") and `systemd --user`.
`deb-native` is that idea ported to Android, where the view and systemd are
unavailable, so the mechanism is inverted:

| | sudo-less (Debian host) | deb-native (Termux/Android) |
|---|---|---|
| base | Debian-on-Debian | Termux (Bionic) **fused** with a userspace native overlay |
| virtualization | kernel view: user+mount namespace + overlayfs | `LD_PRELOAD` path-redirect shim (`native/path-redirect.c`) |
| shell / runtime | the host's own | Termux's pre-existing glibc `bash`/`perl`/coreutils |
| apt / dpkg | a fork retargeted *back* to Debian | Termux's own, reused **as-is** |
| dependency reuse | seed from the host's dpkg db | seed from Termux's installed `*-glibc` packages |

Full side-by-side: [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md).

## Quick start

Requirements: Termux with `clang` and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`/`grun`, `coreutils-glibc`,
`bash-glibc`, `perl`, and the loader/libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) is already in Termux.

```sh
git clone https://github.com/jronminh/deb-native
cd deb-native

./install.sh ~/.dn figlet tree     # bootstrap the base, then install pkgs
exec bash                          # or: . ~/.bashrc
figlet hi                          # installed program, run by name
```

After activation, **Termux's own `apt` and `dpkg` are wired to the real
Debian `arm64` repository and to the prefix** (no wrapper command, no new
name) — the pipeline hangs on apt's own hooks, the way `sudo-less` does:

```sh
apt install cowsay        # Termux's apt -> deb.debian.org -> the prefix
apt remove cowsay
dpkg -i ./some.deb        # direct dpkg targets the prefix too
```

`install.sh` is idempotent: run it again with more packages, or point it at
a different prefix. Under the hood:

- `scripts/setup-apt-prefix.sh` — point `apt` at a Debian `arm64` repo,
  scope it to the prefix, bootstrap the base set, generate launchers.
- `scripts/apt-install.sh` — resolve + download with `apt`, patch each
  `.deb`, drive `dpkg --unpack` / `--configure` one package at a time.
- `scripts/make-launchers.sh` + `scripts/dn-activate.sh` — wrappers + `PATH`.

## What works today

- Fresh bootstrap of the Debian base: `base-files`, `base-passwd`, `dash`,
  `debianutils`, `debconf`, `cdebconf`, `openssl`, `ca-certificates`,
  `mawk` and their libraries — 28 packages to `ii`.
- Installing real leaf packages and their dependency chains from
  `deb.debian.org`, with Debian dependencies that Termux's glibc install
  already provides reused in place (no duplicate files).
- Programs that read hardcoded absolute paths (`/usr/share/figlet`, …) run
  correctly once launched through the overlay.
- Programs run by name from a new shell.

## What it does not do

- **No fake root.** No uid-0 illusion (`fakeroot`/`proot -0`); files keep
  the real unprivileged uid, and `dpkg` runs `--force-not-root`.
- **No isolation.** The kernel here cannot build a user namespace, so an
  install does not hide `$HOME` or provide an empty `/run` the way
  sudo-less's view does. Run only packages you trust.
- **No system services yet.** No `systemd --user` on Termux; `runit`
  (`termux-services`) is the candidate ([research](docs/services-research.md)).
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

- [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) — the method, side by side
  with `sudo-less`.
- [`docs/design-manual-overlay.md`](docs/design-manual-overlay.md) — the
  userspace overlay, and why the kernel view is unavailable here.
- [`docs/design-native-deps.md`](docs/design-native-deps.md) — reusing
  Termux's glibc packages instead of duplicating them.
- [`docs/design-install-path.md`](docs/design-install-path.md) — why
  Termux's apt/dpkg are reused rather than patched.
- [`docs/design-static-wrappers.md`](docs/design-static-wrappers.md),
  [`docs/design-hooks.md`](docs/design-hooks.md) — run-time wrappers.
- [`docs/findings-runtime-and-base-2026-09-25.md`](docs/findings-runtime-and-base-2026-09-25.md)
  — how the base bootstrap was made to work end to end.
- [`docs/findings-shim-perf-2026-09-25.md`](docs/findings-shim-perf-2026-09-25.md)
  — the shim's performance and the one-time cost of `grun`.
- [`docs/prior-art.md`](docs/prior-art.md) — what carries over from
  `sudo-less`, and related work (`proot`, `proroot`).

## Prior art / credit

- [`sudo-less`](https://github.com/jronminh/sudo-less) — the prefix-install
  approach and its documentation are the starting point; the `apt`/`dpkg`
  patches both projects build on originate in Termux.
- Termux and `termux-pacman/glibc-packages` — the Bionic host and the glibc
  userland the fusion relies on.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
