# Debian mode: Termux as a bare platform, Debian as the only supply

Experiment on the `fusion-debian-mode` branch (built on `fusion-no-prefix`).
Instead of making Termux's and Debian's packages coexist in one dpkg
database, a switch turns Termux's own repositories **off** (not deleted) and
makes Debian `arm64` the only package source. Termux is reduced to what it
already is underneath: an Android app with a Bionic userland, a real
(patched) `apt`/`dpkg`, and a glibc side-install. Debian `.deb`s go through
the pipeline `main` already built (repack, maintainer-script patching, ELF
patching, launchers, the path shim) and land fused into `$PREFIX`.

## Why: what the coexistence route ran into

All measured on a real device (Termux dpkg 1.22.6 / apt 2.8.1, `aarch64`
native + `arm64` foreign), mostly as `apt-get -s` against a *copy* of
Termux's status and a real Debian stable `arm64` index:

1. **dpkg keys packages by name.** A second architecture of the same name
   can only be co-installed when *both* are `Multi-Arch: same` and every
   shared file is byte-identical. Otherwise installing `foo:arm64` silently
   **crossgrades** `foo:aarch64` away (`Remv jq`, `Remv perl`). Termux ships
   no `Multi-Arch` fields, and 914 package names exist in both archives (74
   of them installed on the test device).
2. **`Architecture: all` means native.** About half of Debian (33,690 of
   68,196 packages) is `all`, which apt/dpkg treat as the *native*
   architecture — Termux's side. So Debian's `all` packages share a name
   space with Termux's, and their dependencies (`perl:any`, `perl-base`,
   `python3-certifi`) must be satisfied by Termux. Rewriting
   `all` -> `arm64` fixes that part (verified: `moreutils`, `git` become
   resolvable), but only exposes (1) at full scale: Debian's base
   (`dpkg`, `tar`, `openssl`, `perl`, ...) collides with Termux's.
3. Solving (1) would mean renaming ~900 packages and every relation that
   points at them, *and* relocating their files — rebuilding a separate
   name space inside the shared one.

Debian mode sidesteps all three: with only one supply active, there is
nothing to arbitrate between.

## What stays from Termux: the platform

Termux's installed packages remain installed and keep working; they are
simply not *upgraded* while Debian mode is on. They are the platform:
`apt`/`dpkg` themselves, the Bionic shell and coreutils the pipeline's
scripts run on, `termux-exec`, and the glibc side-install.

### libc6 is the one package that cannot come from Debian

Tested on the device: Debian's own `libc6` (glibc 2.41) loaded through its
own `ld.so` kills every binary at startup with `SIGSYS` (exit 159;
`strace` shows `si_syscall=__NR_set_robust_list`) — Android's app seccomp
filter. The same `hello` binary runs under Termux's glibc.

Termux's glibc (`termux-pacman/glibc-packages`, `gpkg/glibc/`) is Debian's
libc translated for Android **at the source level**, ~50 patches:
`set-nptl-syscalls.patch` (drops `set_robust_list`), `disable-clone3`,
`disable-termios2`, `fakesyscall.json` (blocked syscalls mapped to allowed
ones; `setuid*`/`setgid*` return 0), SysV `shm*`/`sem_open` emulation,
Android passwd/group NSS, `set-dirs.patch` (paths under `$PREFIX/glibc`).
None of this is reachable from outside `libc.so.6` — no repack, ELF patch
or `LD_PRELOAD` shim can change what libc does internally.

`grun` (glibc-runner) was read in full: ~280 lines of bash that only
`unset LD_PRELOAD`, prepend `$PREFIX/glibc/bin` to `PATH`, `patchelf
--set-interpreter/--set-rpath` (`--configure`), and optionally preload
`libtermux-exec-glibc.so` (`--teg`). All the Android work is in the libc
build, not in the launcher.

So Debian's `libc6` is satisfied by Termux's glibc (2.44 >= Debian's
2.41, the compatible direction), exactly as `main`'s `native-seed.sh`
already maps `libc6:glibc`. Every other package can come from Debian.
(The syscall tracer can also run stock Debian glibc, since it answers any
seccomp-blocked call with `ENOSYS` — that remains the later optimization
step documented in `docs/syscall-boundary.md`, not this experiment.)

## The switch

`scripts/dn-mode.sh debian|termux|status`. One file is the switch:
`$PREFIX/etc/apt/apt.conf.d/99-dn-debian-mode`. Present = Debian mode,
absent = stock Termux. Termux's own `sources.list`, `sources.list.d/`,
lists and keys are never edited or deleted.

The snippet points apt at mode-private state, so the two modes never
overwrite each other:

| Setting | Debian mode value | Why |
|---|---|---|
| `Dir::Etc::SourceList` / `SourceParts` | `$PREFIX/etc/deb-native/debian-mode/sources.list(.d)` | Termux's sources untouched |
| `Dir::State::Lists` | `$PREFIX/var/lib/apt/lists-debian/` | `apt update` would otherwise delete Termux's lists |
| `Dir::Cache` | `<termux cache>/apt-debian` | separate `pkgcache.bin` / archives |
| `Acquire::PDiffs` | `false` | the index is rewritten after download (below); a pdiff against a rewritten file would fail its hash |
| `APT::Update::Post-Invoke-Success` | `scripts/dn-debian-index.sh` | the `all` -> `arm64` index rewrite |
| `DPkg::Pre-Invoke` | guard | refuses to run dpkg until the install pipeline is wired in (next step) |

apt reads `apt.conf.d` *after* `APT_CONFIG`, but `main`'s classic prefix
config sets its own `Dir::Etc`, so its parts directory is its own and this
snippet never leaks into `~/.dn`. `pkg` goes through apt and therefore
follows whichever mode is active.

### Index translation: `Architecture: all` -> `arm64`

`scripts/dn-debian-index.sh` rewrites every `binary-arm64_Packages` list
after each successful `apt update`. Debian's `all` packages then live on
the `arm64` side and resolve dependencies against Debian, never against
Termux's same-named `all` packages. The `.deb` hashes in the index are left
untouched, so download verification still checks the original files; the
repack step (next) must rewrite each `.deb`'s control file by the **same
rule**, so apt and dpkg agree on what was installed.

### Sources

Debian stable `main` (+ `stable-updates`, `stable-security`), marked
`[trusted=yes arch=arm64]` like `main`'s prefix — Termux has no Debian
keyring. This is a known gap inherited from `main`, not solved here.

## Install pipeline (next step, not wired yet)

| apt hook | Step | Source |
|---|---|---|
| `DPkg::Pre-Install-Pkgs` | control rewrite (`all` -> `arm64`), flatten `usr/` into `$PREFIX`, maintainer-script patching, refuse on file collision | `fuse-repack.sh`, `patch-deb.sh`, `fuse-classify.sh` |
| `DPkg::Pre-Install-Pkgs` | plan guard: refuse any run that removes an `aarch64` package | new |
| `DPkg::Post-Invoke` | ELF patching (scoped per package), symlink normalizing, launchers | `fuse-patch-elfs.sh`, `normalize-symlinks.sh`, `make-launchers.sh` |
| one-time | `libc6:arm64` (+ glibc library packages) registered as provided by Termux's glibc | `native-seed.sh` mapping |

First target package: `sl` (resolved cleanly in the dry run: 9 Debian
libraries, no Termux package removed).

## Backup and restore

Before the first switch, the whole Termux package state was archived on the
device: `~/dn-fusion-backup/termux-pkgstate-<ts>.tar.gz` (+ `.sha256`,
`MANIFEST-<ts>.txt`) — `etc/apt`, `etc/termux`, `etc/alternatives`,
`var/lib/dpkg`, `var/lib/apt`. Restore: `cd $PREFIX && tar -xzf <archive>`.
Switching back is `dn-mode.sh termux` alone; the archive is for undoing
installs, not the switch.

## Known interactions

- `main`'s arch-aware `apt` wrapper (`~/.dn/usr/lib/deb-native/bin/apt`,
  first on an activated shell's `PATH`) decides "Termux has it?" by asking
  the Termux-side apt. In Debian mode that question is answered from
  Debian's index, so installs route to the fused side, and `apt update`
  also refreshes `~/.dn`. Harmless, but use `$PREFIX/bin/apt` to be
  explicit while experimenting.
- Termux upgrades (including security fixes) need `dn-mode.sh termux`
  first.
