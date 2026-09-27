# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/design.md`, "Order
of work"), and judged by its criterion: **per-section coverage from a
random sample**, not feature count. See
[`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) for the side-by-side diff
and [`docs/findings.md`](docs/findings.md)
for what just landed.

## Hotfix v0.2.1-prealpha

Bugs found in the 0.2.0 release, collected for a patch release.

- [x] **`chroot` in maintainer scripts dies with SIGSYS** (released in v0.2.1-prealpha) -- a package whose
      postinst uses Debian's `chroot "$DPKG_ROOT"` pattern (`dbus`, and so
      `ipp-usb`, `avahi-daemon`) failed to configure, and every later apt
      run then ended in "Errors were encountered", even when the requested
      package installed. **Temporary fix:** priv `chroot` runs the command
      directly when the target is the prefix. Not yet verified on a device.

## Alpha goal (the next announcement)

Pre-alpha means "works for the author"; alpha means "try it, it mostly
works". Alpha is reached when all **must have** items are done, whatever
the version number by then. Announce it with one number ("N of 100 random
Debian packages install and run, no root, no proot"), the comparison with
proot-distro (README), and a `gcc` demo.

**Must have** -- what people try first, and what skeptics ask:
- [ ] **Compilers.** `libc6` as Debian's exact identity (below, "Next");
      `gcc`, `make` and a C hello-world build and run in the prefix; `ghc`
      or `rustc` as a bonus.
- [ ] **Popular languages.** `python3` with a C-extension package (e.g.
      `python3-numpy`), `perl` with an XS module, `ruby`, `nodejs` -- incl.
      the Perl version gap (`dn-perl` is Termux's 5.42, trixie builds for
      5.40).
- [ ] **Unfiltered survey.** The seeded 100 across the 43 sections without
      the size filter: a number for heavy packages, no asterisk
      ([`docs/survey-0.2.0.md`](docs/survey-0.2.0.md) is lightweight only).
- [ ] **More than one device.** A second phone / Android version, and the
      Google Play build of Termux.

**Should have** -- makes it feel finished:
- [ ] **Upgrade and remove.** `apt upgrade` across a Debian point release;
      `install.sh --uninstall` (`~/.dn` + the `~/.bashrc` block); Termux
      untouched afterwards (0.2.0 build order, step 8).
- [ ] **Clear refusals.** An out-of-scope package says why ("needs a system
      service", "needs root") instead of a raw dpkg error.
- [ ] **Demo.** `apt install gcc`, compile, run -- all inside Termux.

**To consider: auto-adopt downloaded glibc programs.** `dn-adopt` (manual,
done) makes a glibc program obtained outside apt run through the prefix:
Claude Code's native binary (Bun, 240 MB) runs once adopted. Auto-adopt
would do it when the program is started, so direct installers
(`curl ... | bash`) land with no manual step. Acceptance test: Claude
Code's installer finishing by itself.
- **A. Inside the prefix** (`dn-shell`, programs started by prefix
  programs): the shim's `execve` handling adopts a glibc program whose
  loader does not exist, then starts it. Contained; reuses `dn-adopt.sh`.
- **B. All of Termux**, opt-in (`install.sh --auto-adopt`): a small Bionic
  preload beside `termux-exec` doing the same check -- covers plain
  Termux shells, but every program start passes through it.
- Adopt **in place** (keeps `/proc/self/exe`, needed by Bun/Node
  single-file builds), not "run through the loader". Say so on stderr
  (`deb-native: adopted FILE`); opt-out `DN_NO_AUTO_ADOPT=1`; unwritable
  file -> run through the loader with a warning; first start of a big
  binary takes seconds; self-verifying updaters see a changed file. State
  plainly in the README that downloaded executables get modified.

**After alpha:** services (runit translation), `sudo` modes, busybox base,
the pre-translated repo.

## 0.2.0-prealpha roadmap: a self-contained prefix

Goal: the prefix is a small, complete Debian system of its own -- its own
`apt`/`dpkg`, database, `libc6` and Debian base, preinstalled at bootstrap
-- so installing into it is "just apt", as on Debian (the principle
`sudo-less` uses and the [`naibed`](https://github.com/jronminh/deb-native/tree/naibed)
branch proved on Termux: apt and dpkg own their root). Self-contained for
installing; a guest of Termux for running (shim, `dn-shell`, launchers,
`dn-run`). Termux is never touched. Build on `dev-0.2.0`; design notes in
[`docs/design-0.2.0.md`](docs/design-0.2.0.md) (to be brought in line
with the decisions below).

**Decided**

- **Prefix = a real Debian root, nested** (`$DN/usr/bin`, `$DN/etc`,
  `$DN/var`, `$DN/home`, `$DN/root`, ...). No `usr -> .` flattening:
  `naibed` paid for that one (inverted merged-/usr check, dropped
  `bin -> usr/bin` links, `usr/usr` doubled paths, the `var/run` clash,
  special cases in the shim, `dn-launch`, `dn-run`). In a root of its own,
  `base-files` makes `bin -> usr/bin` itself, as on Debian.
- **apt/dpkg: Termux's own, through launchers** -- no build toolchain, no
  rebuilt packages, Termux updates carry through. Prefix commands set
  `APT_CONFIG`, `--admindir`, `--instdir` explicitly, never `DPKG_ROOT`
  alone. Stand-in packages `dpkg`/`apt` in the prefix's database, versioned
  like Termux's.
- **Reuse naibed's code base** (install pipeline and runtime pieces), minus
  what only the flat layout or sharing Termux's database needed:
  - reused: hook pipeline (per-`.deb` repack, collision check,
    `patch-deb.sh`, per-package `custom/` fixes); ELFs repointed during
    repack at the `libc6` stand-in (`$DN/usr/lib/ld-linux-aarch64.so.1`,
    `RUNPATH` `$DN/usr/lib/aarch64-linux-gnu`); `arm64` as a foreign
    architecture + `Architecture: all` -> `arm64` (index and control);
    `libc6` stand-in -> Termux's glibc; `update-alternatives` wrapper
    (`--log` via `DPKG_ROOT`) and `dpkg-divert` wrapper;
    `dn-fix-alternatives.sh`; scoped launchers incl. alternatives links.
  - dropped: `usr/` flattening, merged-/usr link dropping, `base-files`
    customization, `DN_FUSE_USR` / `fusion-bin` / fusion paths in `dn-launch`
    and `dn-run`, the floor guard, `dn-dash`/`dn-openssl`/`dn-ca-certificates`
    (a separate database has no name clashes: Debian's own are used).
- **Termux's home linked as the prefix's `/root`** (`$DN/root` -> Termux
  home): `base-passwd`'s only user is `root` with home `/root`, maintainer
  scripts assume root, and a later fake-root `sudo` resolves root's home.
  No `$DN/home` (not mapped, see below). Needs the shim to rewrite
  `/root` too (today: `/usr`, `/etc`, `/var`, `/opt` only). Check at
  bootstrap that `base-files` does not drop a `.profile` into Termux's home
  (done: `custom/base-files.sh` skips it, and the prefix keeps only
  `usr etc var opt`, `bin lib sbin` and `root`).
- **Retired from main:** `native-seed.sh` (stub entries), `--force-architecture`,
  post-install `patch-elfs.sh`, `apt-install.sh`'s one-at-a-time loop.
- **Kept from main:** runtime layer, `termux-dn-doctor`.
- **`apt`/`dpkg` are the prefix's** in the user's interactive shell (aliases in
  the managed `~/.bashrc` block); Termux's are `pkg` (as Termux recommends),
  `termux-apt`, `termux-dpkg`. Aliases never reach scripts, so `pkg` keeps
  calling Termux's real apt/dpkg. Replaces 0.1.x's "Termux wins" routing
  wrappers. Removal: `sed -i '/# deb-native/d' ~/.bashrc`.

**Open**

- [ ] Prefix location: keep `~/.dn` (inside Termux's home, so
      `$DN/root` -> `~` loops: `~/.dn/root/.dn/root/...`, harmless for
      normal `find`/`du`, not for `-L`), or move it out of `$HOME`, e.g.
      `/data/data/com.termux/files/dn`.
- [ ] `dpkg-trigger` under `DPKG_ROOT`: check when a trigger-using package
      comes through; wrap like `dpkg-divert` if it double-prefixes.
- [ ] `dpkg --print-architecture` answers `aarch64` inside the prefix;
      watch for maintainer scripts that expect `arm64`.

**Build order**

- [x] 1. Prefix bootstrap: directories, `etc/apt` config (Debian sources,
      `arm64` foreign, no Recommends, no pdiffs, hooks), prefix
      `apt`/`dpkg` launchers, index `all` -> `arm64` rewrite.
- [x] 2. Stand-ins `libc6`, `dpkg`, `apt`; pins (-1) on Debian's `libc6`,
      `libc-bin`, `libc6-dev`, `libc-dev-bin`, `libc-l10n`, `locales`,
      `dpkg`, `apt`, `sudo`, `doas`.
- [x] 3. Hooks from naibed, adapted to the nested root (pre: control
      rewrite, ELF repoint, `custom/`, collision check, `patch-deb`;
      post: alternatives, symlinks, scoped launchers, stale launchers).
- [x] 4. Runtime: naibed's wrapper fixes in `setup-runtime.sh`; shim
      rewrites `/root`; `$DN/root` -> Termux home.
- [x] 5. Debian base through the prefix's own apt (`mawk base-files
      base-passwd dash debianutils debconf cdebconf openssl
      ca-certificates`), then held.
- [x] 6. `install.sh` and routing on top; retire the main pieces above.
- [x] 7. Verify **on a vanilla Termux**: fresh install (3m13s) and the
      100-package survey ([`docs/survey-0.2.0.md`](docs/survey-0.2.0.md):
      99 install, 98 run within the survey's limits, the other 2 fixed);
      `apt-get check` clean.
- [x] 8. Delete the prefix and confirm Termux is untouched (fe2,
      2026-09-27: `rm -rf ~/.dn` + the `~/.bashrc` lines; Termux's
      `sources.list`, `apt.conf.d`, foreign architectures and dpkg database
      free of deb-native, `apt-get check` clean, `apt`/`dpkg` Termux's).
- [x] 9. priv `chroot` (**temporary**, hotfix 0.2.1):
      `dbus-system-bus-common`'s postinst runs `chroot "$DPKG_ROOT" ...`,
      which Android's seccomp kills (SIGSYS, exit 159). A chroot into the
      prefix now runs the command directly (the shim already makes the
      prefix `/`); any other root is refused. To be replaced by the
      identity/services layer ("After alpha"), which also creates the
      system users those scripts go on to add.

**Next release (not 0.2.0): the repo** -- the same translation at repo
build time in [`deb-native-repo`](https://github.com/jronminh/deb-native-repo)
(private): packages arrive translated and signed by deb-native (the Debian
sources themselves are verified since 0.2.0: the bootstrap fetches
`debian-archive-keyring` and checks it against pinned fingerprints),
the device hooks stay as a fallback.

**Next (0.2.x): `libc6` as Debian's exact identity.** The stand-in wraps
Termux's patched glibc but is versioned like Termux's (`2.44-0dn1`), so
`libc6-dev`, which needs `libc6 (= 2.41-12+deb13u4)`, cannot install: every
toolchain is blocked (survey run 1, via `ghc`). Plan:
- [ ] stand-in version = Debian's current `libc6` (read from the prefix's
      index at bootstrap), Termux's real version recorded in the package
      (e.g. `X-Termux-Glibc: 2.44`);
- [ ] rebuilt after `apt update` when Debian's `libc6` version moves (the
      index hook), so point releases do not break it again;
- [ ] revisit the -1 pins on `libc6-dev`, `libc-dev-bin`, `libc-bin`,
      `locales`: Debian's own are fine once `libc6` matches (only `libc6`
      itself stays the stand-in);
- [ ] verify: `gcc` hello-world in the prefix (`libc6-dev`'s linker script
      names `/lib/aarch64-linux-gnu`, which the shim does not rewrite);
- [ ] re-run the unfiltered survey sample for a number on heavy packages;
      check Perl XS modules (`dn-perl` is Termux's Perl 5.42, trixie builds
      for 5.40).

**After 0.2.0, in order: services, then sudo.** Same scope as sudo-less;
a service needs something to run it and the rights it expects.
- [ ] 1. **runit translation**: a real `update-rc.d`/`invoke-rc.d` (and
      `deb-systemd-helper`) in priv that turns a package's init script or
      systemd unit into a termux-services (runit) service under the prefix,
      started by `sv`, at boot through Termux:Boot -- deb-native's
      counterpart of sudo-less's `systemd --user` translation. Until then
      they are no-ops: a package that ships a service installs, the service
      does not run.
- [ ] 2. **sudo modes** (below): fake root + `base-passwd`'s users so
      `adduser`/`chown service-user` in maintainer scripts succeed.
- [ ] 3. Both together: packages that need a system user *and* run a
      daemon -- sudo-less's service support, matched.

**Idea for later, not in 0.2.0: busybox as the maintainer-script toolbox.**
Debian's dynamic `busybox` (glibc, so ld-dn + shim, no tracer) with its
applet links in `priv/` could replace the base's script tools (`coreutils`,
`sed`, `grep`, `findutils`, `diffutils`, `gzip`, `tar`, `mawk`,
`debianutils`, `hostname`); a package depending on one of them then pulls
the real one, keeping the dpkg db honest (no fake stand-ins). Before
deciding: time the bootstrap per stage to see what those packages cost,
then install packages with real maintainer scripts on a busybox-only base
(GNU-only options such as `sed -z`, `grep -P` are the risk).
`busybox-static` would need the tracer on every call -- not that one.

**Prepared for later, not in 0.2.0: `sudo` in the prefix.** It never
means Android root. Three kinds, stackable: pass-through (installers that
just prefix `sudo`), fake root (`fakeroot`-style: uid 0 believed, ownership
recorded), a real extra identity (Android's shell uid via
`termux-adb-bridge`, or a bounded identity as in `dsb`). In 0.2.0 only:
- [x] pin Debian's `sudo`/`doas` to -1 (setuid-root binaries that cannot
      work here, and the name is reserved for ours);
- [x] group the no-op `chown`/`chgrp`/`dpkg-statoverride` into one privilege
      layer (`$DN/usr/lib/deb-native/priv/`) a later mode can replace
      (now also `getent`, `update-rc.d`, `invoke-rc.d`,
      `deb-systemd-helper`, `deb-systemd-invoke`);
- [ ] keep `dn-run`/`dn-shell` preload handling general enough for a
      second `LD_PRELOAD` (a fake-root library beside the shim);
- [ ] keep `base-passwd`'s `root` user and `sudo` group as Debian has them.

## Done recently

- [x] **Fixed fresh bootstrap being broken outright** (`setup-apt-prefix.sh`
      never called `setup-runtime.sh`, so `make-launchers.sh` died on a
      missing `path-redirect.so`) — the existing-prefix reuse path hid it.
- [x] **Fixed cross-prefix apt/dpkg hijacking** — internal scripts called
      bare `apt-get`/`dpkg`, which resolve through `PATH` to ANOTHER
      already-activated prefix's arch-aware wrapper instead of the real
      binaries. Also: `apt remove`/`purge`/`reinstall` routed by repo
      *availability* instead of actual *install location*, so a name
      packaged by both Termux and Debian (`bc`, `tree`) had no working
      `apt remove` for the prefix-installed copy.
- [x] **Hardened every prefix/instdir path argument to absolute** — none of
      the pipeline scripts defended against a relative path, even though
      each one's usage comment documents direct invocation; a relative path
      could corrupt `apt.conf` or get baked into a generated wrapper/shebang.
- [x] **`dn-activate.sh` warns on prefix swap** instead of silently
      replacing which prefix is on `PATH`.
- [x] **Flattened the prefix layout** — `$DNPREFIX` is now the instdir
      directly, dropping the nested `root/` subdirectory (removed a
      genuinely empty, never-used duplicate dpkg admindir, and resolved a
      name collision with Debian's own `/root`).
- [x] **`update-alternatives` writes into Termux's own prefix** (root cause:
      it defaults `--altdir`/`--admindir` to its own compiled-in absolute
      path, which the shim correctly never touches) — fixed with a
      generated wrapper forcing the prefix's own directories. Fixes both
      the `figlet` and `awk -> mawk` cases in one place.
- [x] no-op shim for `dpkg-statoverride` (Termux's `dpkg` doesn't ship it;
      `ca-certificates` logged `command not found` but still reached `ii`).
- [x] **`termux-dn-doctor`** — a generated command (in the launcher dir, so it
      runs by name) that checks the Termux↔prefix seams: a leaked
      `APT_CONFIG` in the shell rc (which made `apt update` show only Debian),
      a Termux `sources.list` clobbered by installing into `$PREFIX`, and
      stale/missing wrappers or activation; `--fix` repairs them.
      [`scripts/dn-doctor.sh`](scripts/dn-doctor.sh).
- [x] **Fresh base bootstrap to `ii`** (all 28 base packages) — see
      [`docs/findings.md`](docs/findings.md).
- [x] **Seamless launch**: `scripts/make-launchers.sh` + `scripts/dn-activate.sh`
      — installed programs run by name; `termux-exec` preserved (a Bionic
      child gets it back via `DN_BIONIC_PRELOAD`, a glibc child gets the shim).
- [x] **First full install script**: `install.sh PREFIX [pkg...]` — bootstrap
      if new, reuse if existing, install, generate launchers, activate PATH.
- [x] **Package scope decided + libc-shim coverage measured and completed**:
      [`docs/standard.md`](docs/standard.md) (section-based scope) and
      [`docs/shim-coverage.md`](docs/shim-coverage.md) (258-package in-scope
      corpus). Every imported path-taking symbol is now intercepted except
      NSS lookups (tested: opened inside libc, out of the shim's reach),
      `glob`/`glob64` (indirect), admin ops, and the raw-`syscall()` boundary.

## Now / do first

- [ ] **Re-run the coverage survey on a fresh prefix** (sudo-less's
      yardstick; the base-env fix invalidates the old 33%).
      ```
      python3 scripts/sample-packages.py <packages-file>   # seed 20260925
      OUT=~/survey1 scripts/survey-apt.sh LIST.tsv
      ```
      Report: installed %, and the failure classes (unresolvable /
      maintainer-script / hardcoded-path / other). This decides whether
      the next build is run wrappers (#1) or the classifier (#2).

## Then, in order

- [ ] **Run Tailscale natively (userspace networking)** — the target case for
      fork-lite: a static Go daemon the shim cannot see, needing the tracer.
      Package findings, blockers and the plan are in
      [`docs/tailscale.md`](docs/tailscale.md).
- [ ] **Stage 4 run wrappers** (`prefix-wrap` equivalent) — the biggest
      unbuilt piece. For each binary a package puts on `PATH`, detect
      whether it needs path help (interpreter not present, interpreter's
      compiled-in module path, `ldd`-missing lib, hardcoded `/usr /etc
      /opt` path) and generate a wrapper. Build on
      [`docs/design.md`](docs/design.md);
      trigger via [`docs/design.md`](docs/design.md) (apt
      `DPkg::Post-Invoke` + a `dpkg` wrapper), not a source patch.
- [ ] **Stage 2 classifier / refusal** (`prefix-check` equivalent) — read
      each `.deb` before dpkg runs; classify scope (in / admin's / never)
      and mechanism (none / env / shim), flag unsafe maintainer scripts,
      and refuse a "never" package **before** dpkg can wedge the prefix.
- [ ] **Stage 1 sync on `apt update`** — keep the prefix's view of
      Termux's `*-glibc` packages fresh (`APT::Update::Post-Invoke-Success`),
      so a Termux upgrade doesn't leave stale seeds / "held broken
      packages".
- [ ] **Patch B: two-layer database, soname-based** — replace
      `scripts/native-seed.sh`'s hand-written ~10-entry name table with
      matching a `.deb`'s `Depends:` against installed `*-glibc` packages'
      `.so` SONAMEs (`docs/design.md`, "Open work"). Small,
      directly improves install success.
- [ ] **Stage 4 integration** — launchers/icons/desktop DB (mostly N/A on
      Android; do only what Termux needs).
- [ ] **Direction 3: services on `termux-services` (runit)** — translate a
      package's systemd unit into a runit `run` script
      (`docs/design.md`). The service *view* (config/state at
      `/etc/foo`, `/var/lib/foo`) is the hard part and shares Direction 2's
      unsolved gap.
- [ ] **State + `explain` + `doctor`** — record per package its scope,
      mechanism and the wrappers it got, so decisions are explainable and
      removable; `sudo-less` doesn't have these either.

## Quick wins

- [ ] **forked Bionic `sed`/`find`** in some postinsts can't see prefix paths
      (`ca-certificates` logs `sed: can't read /etc/ca-certificates.conf` but
      still reaches `ii`). The shim only covers the glibc shell's own calls;
      install Debian `sed`/`findutils` or wrap them.
- [ ] refresh `README.md` status numbers once the survey (#Now) reports.

## Shim hardening (from code review + platform probes, 2026-09-26)

Tracked in [#1](https://github.com/jronminh/deb-native/issues/1).

- [x] **Fix `unlink()`** — fixed in `521cc73` (was declared with
      `unlinkat_t` and called `real(AT_FDCWD, path, 0)`, passing `(char
      *)-100`).
- [x] **Grow the intercepted libc surface** — the issue-#1 gaps landed in
      `521cc73` (`lstat`; `fopen64`/`freopen64`/`openat64`/`fstatat64`/
      `truncate64`; fortified `__open_2`/`__openat_2`/`__open64_2`;
      `statfs`/`statvfs`; `dlopen`/`dlmopen`; AF_UNIX `bind`/`connect`).
      This change finishes the libc layer with the rest of the path-taking
      surface: `creat`/`creat64`/`freopen`; `chown`/`lchown`/`fchownat`;
      `utime`; the xattr family (`setxattr`/`lsetxattr`/`getxattr`/
      `lgetxattr`/`listxattr`/`llistxattr`/`removexattr`/`lremovexattr`);
      `mkfifo`/`mkfifoat`/`mknod`/`mknodat`; `statfs64`/`statvfs64`;
      `realpath`/`canonicalize_file_name`; `inotify_add_watch`; AF_UNIX
      `sendto`; `mkstemp`/`mkostemp`/`mkdtemp`; `posix_spawn`/
      `posix_spawnp`. Verified on-device by `tests/shim-libc/run.sh`
      (asserts each symbol rewrites; `posix_spawn` of a redirected glibc
      binary runs). What this layer **cannot** reach is the tracer's job:
      raw `syscall()`, static binaries, and libc-internal opens that bypass
      the PLT.
- [ ] **Bake the shim into installed ELFs** (see `docs/design.md`,
      "Delivering the shim"): `patchelf --add-needed`/`--add-rpath`, or a
      `DT_AUDIT` module, so the shim survives an empty environment; the
      explicit loader (`ld.so --preload`) is the simpler variant.
- [ ] **Fork-lite tracer** (`dn-trace`) for what libc interposition can't see
      (static binaries, inline `svc`, libc-internal NSS): a reduced,
      arm64-only subset of `termux/proot` (`ptrace` — seccomp user-notification
      cannot rewrite syscall arguments), keeping `proot` as the fallback. Plan
      and phases in [`docs/direct-usage.md`](docs/direct-usage.md); the measured
      boundary is in [`docs/syscall-boundary.md`](docs/syscall-boundary.md).
      Phase 1: prune non-arm64 + extensions, build arm64-only on `fe2`.
      - [x] prune + build AArch64-only on `fe2`; **bind-only fast path**
            (~1.6x stat-dense; `scripts/bench-tracer.sh`, `docs/bind-only.md`).
      - [x] NSS (case 2): route to the tracer + bind `$INSTDIR/etc` over Termux
            glibc's sysconfdir — `tests/tracer-nss/run.sh` PASS.
      - [x] **direct-syscall attribute** (cases 3/4): `scan-direct-syscalls.py
            --trace-list` + `make-launchers.sh` tag `svc`/`syscall` importers
            with `dn-run --trace`, routing them to the tracer instead of the
            shim (`docs/syscall-boundary.md`, "Solved").
      - [x] replace `cli/` with the `dn-trace` front end; build it at setup;
            kernel exec instead of PRoot's loader
            ([`docs/tracer-0.2.0.md`](docs/tracer-0.2.0.md)).
      - [x] unset `LD_PRELOAD` (termux-exec) on every tracer route.
      - [ ] test `dn-run` → `dn-trace` from an installed prefix.
      - [ ] drop (or keep, for installs without `make`/`libtalloc`) the
            Termux `proot` fallback in `dn-run.c`.

## Open, still-unsafe

- [ ] `--force-architecture` workaround for the archive-name mismatch
      (`arm64` vs `aarch64`) — flagged unsafe in
      `docs/findings.md`; decide on a real fix.

## Blocked / impossible on this device

Keep these out of scope. Probed 2026-09-26 (`docs/findings.md`, "Platform
sandbox limits"): user namespaces are off **kernel-wide** (`CLONE_NEWUSER` =
`EINVAL` even from the seccomp-free shell), mount namespaces need
`CAP_SYS_ADMIN`, and `/dev/fuse` is root-only:

- install-view / service-view isolation (hidden `$HOME`, empty `/run`);
- `prefix-sandbox`'s seccomp + namespace isolation;
- other-architecture (i386) loaders and `Multi-Arch` skew;
- setuid/setgid and file capabilities;
- a TUN device, firewall rules, raw sockets.
