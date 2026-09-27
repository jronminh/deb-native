# 0.2.0-prealpha design: a self-contained prefix

Status: **design, being built** on the `dev-0.2.0` branch.

## The idea

The prefix (`~/.dn`) becomes a small, complete Debian system of its own:
its own `apt` and `dpkg`, its own database, its own `libc6` and Debian
base, installed at bootstrap. Installing into it is "just apt", the way it
is on Debian. Termux is never touched; deleting the prefix restores it
exactly, as today.

This is the principle `sudo-less` uses on Debian and the
[`naibed`](https://github.com/jronminh/deb-native/tree/naibed) branch
proved on Termux: **apt and dpkg own their root.** When the place packages
live is apt/dpkg's own `/`, dpkg's normal rules (dependencies, Pre-Depends
order, alternatives, diversions, upgrades) just work, and the glue between
two package managers disappears. `naibed` gets that by taking over
Termux's own prefix (one way, unsafe); 0.2.0 gets it inside a sandbox.

The split stays as in `main`: **self-contained for installing, a guest of
Termux for running.** Programs run through `main`'s runtime layer (path
shim, `dn-shell`, launchers, `dn-run`) on Termux's glibc, because Android
gives an app no namespace "view" to fake paths with.

## What the prefix contains from the start

| Package (prefix's own dpkg database) | What it is |
|---|---|
| `libc6` | stand-in: a real package whose files are links at Debian's libc paths into Termux's glibc (`$PREFIX/glibc/lib`). Debian's own `libc6` is killed by Android's seccomp filter at startup; Termux's glibc is the same library patched for Android at source level. Version = Termux's glibc version. |
| `dpkg`, `apt` | stand-ins for Termux's own `dpkg`/`apt` (below), versioned like Termux's, so `Depends: dpkg (>= ...)` is satisfied |
| `mawk`, `base-files`, `base-passwd`, `dash`, `debianutils`, `debconf`, `cdebconf`, `openssl`, `ca-certificates` | Debian's own (the `bootstrap-base.sh` set), translated at install, then **held** |

A separate database has no name clashes with Termux, so, unlike `naibed`,
`dash`, `openssl` and `ca-certificates` are Debian's own packages. Only
`libc6`, `dpkg` and `apt` are stand-ins.

## apt and dpkg: Termux's own, through launchers (decided)

The prefix uses the `apt`/`dpkg` Termux already has: no build toolchain,
no rebuilt packages, lowest requirements. Termux updates to them carry
through.

- **`$DN/bin/apt*`, `$DN/bin/dpkg*`** are small launchers that run Termux's
  binaries with the prefix given explicitly: `APT_CONFIG=$DN/etc/apt.conf`,
  `--admindir=$DN/var/lib/dpkg`, `--instdir=$DN`. Explicit, never
  `DPKG_ROOT` alone: Termux's tools have Termux's paths compiled in, and
  under `DPKG_ROOT` they prepend the prefix to a path that already contains
  one (the doubled-path bug class, below).
- **dpkg's helpers** compute paths from `DPKG_ROOT` by themselves when a
  maintainer script calls them, so each gets a wrapper forcing its own
  paths (from `naibed`, where each was root-caused with `strace`):
  - `update-alternatives`: `--altdir`/`--admindir`, and `--log
    /var/log/alternatives.log` with `DPKG_ROOT` set -- it joins
    `DPKG_ROOT` onto an explicit `--log` too. **`main` has this bug
    today**: `~/.dn/data/data/com.termux/files/usr/var/log/alternatives.log`
    exists on the test device.
  - `dpkg-divert`: `--admindir`/`--instdir` (it opened
    `$DN$PREFIX/var/lib/dpkg/diversions`).
  - `dpkg-statoverride`: `main`'s no-op (unprivileged, no ownership).
  - `dpkg-trigger`: to check when a trigger-using package comes through.

## Layout: Debian's own, nested (unchanged from main)

`~/.dn/usr/bin`, `~/.dn/etc`, ... exactly as on Debian, merged-/usr links
from `base-files` included. `naibed` flattens `usr/` only because Termux's
own prefix is flat; a prefix of our own has no reason to. The shim keeps
mapping `/usr` -> `$DN/usr`.

## Architecture: relabelled to Termux's native `aarch64`

Termux's dpkg calls this CPU `aarch64` (compiled in); Debian calls it
`arm64`. `main` today makes the prefix's apt claim `arm64` and runs dpkg
with `--force-architecture`, which `docs/findings.md` itself calls "not a
real fix" (it disables the check for any architecture). Instead:

- the Debian index is rewritten after each `apt update`:
  `Architecture: arm64` and `all` -> `aarch64`;
- each package's control file gets the same rewrite before dpkg sees it.

dpkg then sees native packages: no `--force-architecture`, no foreign
architecture, and `all` is no longer special. Name clashes cannot happen
(separate database).

## Install pipeline

Phase A (this release): translation on the device, in the prefix's apt
hooks (`naibed`'s pipeline, minus what only fusion needs):

| Hook | Step |
|---|---|
| `DPkg::Pre-Install-Pkgs` | per `.deb`: control relabel (`aarch64`); ELFs repointed at the `libc6` stand-in (`$DN/usr/lib/ld-linux-aarch64.so.1`, `RUNPATH` `$DN/usr/lib/aarch64-linux-gnu` first, shared libraries too) before any maintainer script runs; `custom/<package>.sh` fixes; maintainer-script shebangs -> `dn-shell` (`patch-deb.sh`) |
| `DPkg::Post-Invoke` | alternatives links made relative at once (`dn-fix-alternatives.sh`); new packages' absolute symlinks made relative; launchers for their programs (and for alternatives links to them); stale launchers dropped |

Phase B (next): the same translation at **repo build time**
([`deb-native-repo`](https://github.com/jronminh/deb-native-repo),
private): packages arrive translated and **signed** (ending
`[trusted=yes]`), hashes match what gets installed, the device hooks stay
only as a fallback for packages not in the repo.

## What goes away in main

- `native-seed.sh` (stub database entries): replaced by the real `libc6`
  stand-in and Debian's own libraries.
- `--force-architecture`: replaced by the `aarch64` relabel.
- `patch-elfs.sh` after install (`grun --configure` on the whole prefix):
  ELFs are repointed in the package, before any script runs.
- `apt-install.sh`'s one-package-at-a-time loop: it exists because patching
  had to happen outside apt; with hooks, plain `apt install` keeps dpkg's
  own Pre-Depends ordering.

## What stays

- The runtime layer: shim, `dn-shell`/`dn-perl`, `dn-run`, launchers,
  `termux-dn-doctor`.
- Routing ("Termux wins"): `apt install X` from Termux reaches the
  prefix's apt for Debian-only packages. (A `dpkg -i` of a relabelled
  `.deb` no longer routes by architecture; route by where the file came
  from, or use the prefix's `dpkg` directly.)

## Build order

1. Port the helper wrappers (`update-alternatives` `--log`, `dpkg-divert`)
   and `dn-fix-alternatives.sh` to `main`'s runtime setup.
2. Prefix bootstrap: directories, `apt.conf` (native `aarch64`, Debian
   sources, hooks), launchers, index relabel, `libc6`/`dpkg`/`apt`
   stand-ins.
3. The hooks (pre: relabel, ELF repoint, custom, `patch-deb`; post:
   alternatives, symlinks, launchers).
4. Base set through the prefix's own apt, then held.
5. `install.sh`/routing on top; `native-seed.sh`, `--force-architecture`,
   `patch-elfs.sh`, `apt-install.sh` retired.
6. Verify on device: the packages `main` verifies today, by name.

Test note: the only test device currently online (`fe2`) runs a `naibed`
(fused) Termux. The prefix's apt reads only its own config, so the fused
Termux does not leak into it, but results should be re-checked on a stock
Termux before the release.
