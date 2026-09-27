# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/design.md`, "Order
of work"), and judged by its criterion: **per-section coverage from a
random sample**, not feature count. See
[`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) for the side-by-side diff
and [`docs/findings.md`](docs/findings.md)
for what just landed.

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
  `$DN/home` stays a normal empty directory. Needs the shim to rewrite
  `/root` too (today: `/usr`, `/etc`, `/var`, `/opt` only). Check at
  bootstrap that `base-files` does not drop a `.profile` into Termux's home.
- **Retired from main:** `native-seed.sh` (stub entries), `--force-architecture`,
  post-install `patch-elfs.sh`, `apt-install.sh`'s one-at-a-time loop.
- **Kept from main:** runtime layer, "Termux wins" routing, `termux-dn-doctor`.

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

- [ ] 1. Prefix bootstrap: directories, `etc/apt` config (Debian sources,
      `arm64` foreign, no Recommends, no pdiffs, hooks), prefix
      `apt`/`dpkg` launchers, index `all` -> `arm64` rewrite.
- [ ] 2. Stand-ins `libc6`, `dpkg`, `apt`; pins (-1) on Debian's `libc6`,
      `libc-bin`, `libc6-dev`, `libc-dev-bin`, `libc-l10n`, `locales`,
      `dpkg`, `apt`, `sudo`, `doas`.
- [ ] 3. Hooks from naibed, adapted to the nested root (pre: control
      rewrite, ELF repoint, `custom/`, collision check, `patch-deb`;
      post: alternatives, symlinks, scoped launchers, stale launchers).
- [ ] 4. Runtime: naibed's wrapper fixes in `setup-runtime.sh`; shim
      rewrites `/root`; `$DN/root` -> Termux home.
- [ ] 5. Debian base through the prefix's own apt (`mawk base-files
      base-passwd dash debianutils debconf cdebconf openssl
      ca-certificates`), then held.
- [ ] 6. `install.sh` and routing on top; retire the main pieces above.
- [ ] 7. Verify **on a vanilla Termux** (the test device's Termux is fused
      by `naibed` and gets reinstalled): `figlet`, `lua5.4`, `tree`, `sl`,
      `hello`, `ncdu` by name; `apt-get check`; delete the prefix and
      confirm Termux is untouched.

**Next release (not 0.2.0): the repo** -- the same translation at repo
build time in [`deb-native-repo`](https://github.com/jronminh/deb-native-repo)
(private): packages arrive translated and signed by deb-native (the Debian
sources themselves are verified since 0.2.0: the bootstrap fetches
`debian-archive-keyring` and checks it against pinned fingerprints),
the device hooks stay as a fallback.

**Prepared for later, not in 0.2.0: `sudo` in the prefix.** It never
means Android root. Three kinds, stackable: pass-through (installers that
just prefix `sudo`), fake root (`fakeroot`-style: uid 0 believed, ownership
recorded), a real extra identity (Android's shell uid via
`termux-adb-bridge`, or a bounded identity as in `dsb`). In 0.2.0 only:
- [ ] pin Debian's `sudo`/`doas` to -1 (setuid-root binaries that cannot
      work here, and the name is reserved for ours);
- [ ] group the no-op `chown`/`chgrp`/`dpkg-statoverride` into one privilege
      layer (e.g. `$DN/usr/lib/deb-native/priv/`) a later mode can replace;
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
      - [ ] replace `cli/` with the `dn-trace` binder, then drop the Termux
            `proot` fallback in `dn-run.c`.

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
