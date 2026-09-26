# deb-native — true fusion

![status: experimental](https://img.shields.io/badge/status-experimental-red)
![one-way](https://img.shields.io/badge/transformation-one--way-critical)

**Transform Termux itself into a Debian `arm64` system — no root, no
`chroot`, no proot.** Debian becomes apt's only source, Debian packages
install straight into Termux's own `$PREFIX` and dpkg database, and Termux
is reduced to the packages it runs on.

This is the second branch of [`deb-native`](https://github.com/jronminh/deb-native).
[`main`](https://github.com/jronminh/deb-native/tree/main) installs Debian
packages into a separate prefix beside Termux; this branch takes `main`'s
core one step further and merges the two into one system.

> [!CAUTION]
> **One-way, experimental, and far less safe than `main`.** A bad package
> can break Termux itself, not just a Debian program: every Debian
> maintainer script runs with write access to Termux's live system. There
> is no switch back — the backup restores apt/dpkg state, not every file
> packages wrote. Termux's own base packages (`apt`, `openssl`, `curl`,
> glibc) get no updates, security fixes included, until Termux's repos are
> brought back (phase 3). Written with AI assistance and not independently
> audited. **Use `main` unless you want exactly this trade, and only on a
> Termux install you can wipe.** Details:
> [`docs/true-fusion.md`](docs/true-fusion.md).

## Install

On a Termux you can afford to lose:

```sh
pkg install git clang patchelf glibc-repo
pkg install glibc-runner bash-glibc coreutils-glibc perl-glibc

git clone -b naibed https://github.com/jronminh/deb-native ~/deb-native-fusion
cd ~/deb-native-fusion
sh scripts/dn-backup.sh     # required: dn-fuse.sh refuses without a backup
sh scripts/dn-fuse.sh       # the one-way step

exec bash -l                # new login shell: launchers on PATH
apt install sl && sl
```

Keep the checkout where it is: apt's hooks run its scripts on every install.
After the transformation Termux's `pkg` refuses (it would rewrite apt's
sources); use `apt`.

## How it works

Every Debian `.deb` assumes a real Debian root. Each layer fakes or fixes
one of those assumptions inside Termux's prefix:

- **Layout.** `$PREFIX/usr -> .`, so Debian's `/usr/bin/x` and Termux's
  `$PREFIX/bin/x` are the same place; packages are repacked with `usr/`
  flattened into the prefix and installed with `dpkg --instdir=$PREFIX`.
- **apt and dpkg.** `arm64` added as a foreign architecture; Debian stable
  as the only source; `Architecture: all` rewritten to `arm64` in the index
  and in each package, so Debian never shares a name space with Termux.
- **libc.** Debian's own `libc6` is killed by Android's seccomp filter at
  startup. Termux's glibc, patched for Android at the source level, is
  installed *as* `libc6:arm64`: a real package whose files are links at
  Debian's libc paths. Every Debian binary is repointed at it.
- **The Debian base** (`base-files`, `base-passwd`, `debconf`, `cdebconf`,
  `mawk`) installed through the pipeline, customized where the flat prefix
  differs (`scripts/fusion-custom/`), then held.
- **Maintainer scripts** run through `main`'s `dn-shell` and path shim, so
  `/etc`, `/usr`, `/var` resolve into the prefix; `update-alternatives` and
  `dpkg-divert` are wrapped, and alternatives links are made relative the
  moment they are written.
- **Programs run by name** through `main`'s launchers and `dn-run`, which
  clear Termux's Bionic preload and load the shim.

### Package tiers

| Tier | What | Protection |
|---|---|---|
| 0. Floor | Termux's Essential packages and their dependencies (117 of 260 on the test device): what the app, apt and dpkg run on | never removed or crossgraded (plan guard) |
| 1. Termux-backed identity | Debian names a floor package fills: `libc6`, `dn-dash`, `dn-openssl`, `dn-ca-certificates` | Debian's originals pinned to -1 |
| 2. Debian base | `base-files`, `base-passwd`, `debconf`, `cdebconf`, `mawk` | held |
| 3. Packages | everything else, from Debian | normal apt |

### The install pipeline

| apt hook | Step |
|---|---|
| `DPkg::Pre-Install-Pkgs` | floor guard on apt's full plan; per package: repack (layout, customizations, `all` -> `arm64`, ELFs repointed), collision check, maintainer-script patching |
| `DPkg::Post-Invoke` | alternatives and package symlinks made relative; launchers for the new programs; stale launchers removed |

## Roadmap

1. **Make Debian run smoothly** on the transformed prefix — *in progress*.
2. **Replace** the non-floor Termux packages with Debian's.
3. **Bring Termux's repos back as the secondary supply**, forced to follow
   Debian's multiarch rules: the roles are switched, Termux's packages have
   to fit the transformed system.

## Status

Verified on one device (Termux, arm64, Debian stable `trixie`): the
transformation, tiers 0–2, and `hello`, `lua5.4`, `ncdu`, `sl`, `figlet`,
`sysvbanner`, `mawk`, `debconf` installed through the pipeline and running
by name; `apt-get check` clean.

Known limits:

- **Runtime:** a replaced Termux program becomes a glibc binary; started by
  full path from Termux's Bionic side it inherits `termux-exec`'s preload
  and fails to load. Launchers cover programs started by name. To solve
  before phase 2.
- The floor gets no updates until phase 3.
- Debian sources use `[trusted=yes]` (Termux has no Debian keyring), a gap
  inherited from `main`.
- `apt-get install --reinstall` of a held base package drops the hold;
  re-run `scripts/dn-base-env.sh` to restore it.

## Documentation

- [`docs/true-fusion.md`](docs/true-fusion.md) — the design, tiers,
  pipeline, and what was verified.
- [`docs/fusion-multiarch.md`](docs/fusion-multiarch.md) and
  [`docs/multiarch-mechanics.md`](docs/multiarch-mechanics.md) — dpkg
  multiarch mechanics, and why coexistence in one database fails.
- [`docs/findings.md`](docs/findings.md) — engineering log (this branch
  absorbs the earlier `fusion-no-prefix` experiments).
- `main`'s design docs apply to the shared core:
  [`docs/design.md`](docs/design.md),
  [`docs/shim-coverage.md`](docs/shim-coverage.md),
  [`docs/syscall-boundary.md`](docs/syscall-boundary.md).

## Credit & license

Built on other people's work — see [`CREDITS.md`](CREDITS.md):

- **[PRoot](https://github.com/termux/proot)** (`proot-me/PRoot`,
  GPL-2.0-or-later) — the `ptrace` syscall-interception core; `tracer/` is a
  reduced fork with its headers kept.
- **[Termux](https://github.com/termux/termux-packages)** and
  [`glibc-packages`](https://github.com/termux-pacman/glibc-packages) — the
  host, the non-root `apt`/`dpkg` patches, and the glibc userland this
  branch presents as Debian's `libc6`.
- **[sudo-less](https://github.com/jronminh/sudo-less)** — the prefix-install
  approach and the `apt`/`dpkg` lifecycle-hook idea.

Written with AI assistance (**Claude Opus 5.5**, **DeepSeek v4.1 Pro**).
GPL-3.0-or-later — see [`LICENSE`](LICENSE).
