# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/spec/design.md`, "Order
of work"), judged by its criterion: **per-section coverage from a random
sample**, not feature count. See [`docs/spec/vs-sudo-less.md`](docs/spec/vs-sudo-less.md)
for the side-by-side diff.

**Format**: each section below has a **Goal** (stable, rarely changes), a
**Status** paragraph (the current, stable truth — when an Open item is
finished, its outcome is folded in here and the item is deleted, not kept
as a checked-off line), and an **Open** list (short, no narrative — the
"why"/investigation history lives in [`docs/log/findings/`](docs/log/findings/README.md)
(chronological engineering log) and the other `docs/*.md` files each
section links to).

## Alpha goal (the next announcement)

**Goal**: pre-alpha means "works for the author"; alpha means "try it, it
mostly works". Reached when every item below is done. Announce with one
number ("N of 100 random Debian packages install and run, no root, no
proot"), the comparison with proot-distro (README), and a `gcc` demo.

**Status**: 0.2.0's foundation and 0.3.0's fake-root are released. This
project's own Android-patched glibc is now the prefix's real `libc6` by
default (0.6.0+s.1, below); `install.sh` no longer uses `dn-standins.sh`'s
stand-in, and `ld-dn` is retired as the interpreter.

**0.6.0+s.1 (released, 2026-10-03)**: a fresh prefix is `dn-glibc` by
default -- it fetches the glibc bundle, installs Debian's real
`libc6`/`libc-bin`, then swaps in this project's own-built, Android-patched
glibc (the fused loader + the 10 files) and the shim via
`etc/ld.so.preload`; `ld-dn` is no longer the interpreter. Known open item:
the static `ldconfig` writer has no run-time prefix derivation yet, so the
bootstrap bypasses it (tracer + explicit `-C`/`-f`, non-fatal) until it gets
its own -- `docs/spec/deploy.md` "Open items", and `tests/glibc-swap` is the
acceptance test for this deploy.

**Compilers: done, 2026-10-01.** `apt install gcc` installs and configures
cleanly (`gcc`/`cpp`/`binutils` and their whole dependency chain);
`libc6-dev` installs unmodified from Debian's real archive (no custom
packaging needed -- `dn-package-glibc.sh` keeps the own-built `libc6`'s
version string an exact match instead); the shim's redirect scope now
covers `/lib`/`/bin`/`/sbin`; and `gcc`'s own default dynamic linker is
repointed at `ld-dn` via a generated `specs` file
(`scripts/install/dn-fix-gcc-specs.sh`, wired into `dn-hook-post.sh`) --
GCC's own site-customization hook, no gcc/binutils patch or rebuild
needed. `gcc -o hello hello.c && ./hello` now compiles **and runs**
end to end (`docs/log/findings/gcc-hello-pt-interp-gap.md` has the full chain of fixes). `make`
untested; `ghc`/`rustc` as a bonus, unresearched.

**Open**:
- **Popular languages**: `python3` + a C-extension package, `perl` + an XS
  module, `ruby`, `nodejs` — incl. the Perl version gap (`dn-perl` still
  uses Termux's 5.42; trixie's `perl` builds modules for 5.40, no fix yet).
- **Unfiltered survey**: the seeded 100 across all 43 sections without the
  size filter — a number for heavy packages, no asterisk
  ([`docs/log/survey-0.2.0.md`](docs/log/survey-0.2.0.md) is lightweight only).
- **More than one device**: a second phone / Android version, and the
  Google Play build of Termux.
- **Upgrade and remove**: `apt upgrade` across a Debian point release;
  `install.sh --uninstall`; Termux untouched afterwards.
- **Clear refusals**: an out-of-scope package says why ("needs a system
  service", "needs root") instead of a raw dpkg error.
- **Demo**: `apt install gcc`, compile, run — all inside Termux.

**To consider, not scheduled: auto-adopt downloaded glibc programs.**
`dn-adopt` (manual, done) makes a glibc program obtained outside apt run
through the prefix (e.g. Claude Code's own Bun binary). Auto-adopting on
first run (so `curl ... | bash` installers land with no manual step) has
two shapes — **A**: inside the prefix only (shim's `execve` handling,
contained, reuses `dn-adopt.sh`); **B**: all of Termux, opt-in
(`install.sh --auto-adopt`, a Bionic preload beside `termux-exec`, covers
plain Termux shells but every program start passes through it). Either
way: adopt in place (keeps `/proc/self/exe`), announce on stderr, opt-out
`DN_NO_AUTO_ADOPT=1`, state plainly in the README that this modifies
downloaded executables.

**After alpha**: services + sudo modes (own section below), the
pre-translated repo, 0.4.0's lighter base (own section below), and *true
fusion rebuilt on the 0.2 core* (the `naibed` branch's `ld-dn`/`dn-trace`/
translator/priv layer, with Termux's prefix as the root — frozen until
alpha).

## 0.2.0: a self-contained prefix (released)

**Goal**: the prefix is a small, complete Debian system of its own — its
own `apt`/`dpkg`, database, `libc6` and Debian base — so installing into
it is "just apt", as on Debian. Self-contained for installing; a guest of
Termux for running (shim, `dn-shell`, launchers, `dn-run`). Termux is
never touched.

**Status**: released and verified on a vanilla Termux — fresh install,
the 100-package survey (99 install / 98 run,
[`docs/log/survey-0.2.0.md`](docs/log/survey-0.2.0.md)), a full prefix delete
leaving Termux untouched. Design decisions (real nested Debian root, no
`usr ->.` flattening, Termux's own apt/dpkg through launchers,
`$DN/root` -> Termux home) are recorded in
[`docs/spec/design.md`](docs/spec/design.md). The hotfix that shipped
after (`priv chroot` for maintainer scripts using
`chroot "$DPKG_ROOT"`, e.g. `dbus`/`ipp-usb`/`avahi-daemon`, which
Android's seccomp otherwise SIGSYS-kills) is a temporary fix, still
standing until the identity/services layer replaces it.

**Tried 2026-10-01, reverted: apt/dpkg as real base packages instead of
Termux's borrowed binaries.** Motivated by the same principle already
applied to `bash`/`dash` this session (self-contained > borrowed for
bootstrap scaffolding), and by a real architectural itch: `apt`/`dpkg`
are the one thing in the prefix that never goes through `ld-dn`/the shim
at all. Two approaches tried, both hit a real, hard blocker:
- **Real Debian `apt` 3.0.3/`dpkg` 1.22.22** (`apt-get download`,
  translated and `dpkg -i`'d like any other package): installed clean,
  DNS/downloads worked (after also symlinking a missing
  `$DN/etc/resolv.conf` -- our own glibc's `libnss_dns` needs a real one,
  unlike Termux's Bionic resolver; a `native-glibc-network-client` gap
  worth remembering independent of this attempt), but every actual
  package install failed: apt's `DPkg::Pre-Install-Pkgs`/debconf hooks
  run via a **hardcoded, unconfigurable `Args[0] = "/bin/sh"`** in apt's
  own C++ (`apt-pkg/deb/dpkgpm.cc`, `apt-private/private-json-hooks.cc`
  -- no `Dir::Bin::sh`-style override exists). `/bin/sh` on this device is
  Android's real, foreign Bionic `/system/bin/sh` (not this project's
  `dash`), and it fails to start under the exec chain our own real
  `apt`/`dpkg` create (`CANNOT LINK EXECUTABLE "/bin/sh": ... at
  .../libc.so.6`) -- confirmed **not** an `LD_PRELOAD`/`LD_LIBRARY_PATH`
  leak (a full `envp` dump showed the shim's `bionic_env()` correctly
  stripping both; root mechanism still unknown). Termux's own apt build
  patches exactly this line to `@TERMUX_PREFIX@/bin/sh`
  (`termux-packages` `packages/apt/0004-no-hardcoded-paths.patch`) --
  confirming this is a real, known-since-2020 incompatibility, not
  something specific to this project.
- **Termux's own real `apt`/`dpkg` `.deb`s** (already correctly patched,
  downloaded via `apt-get download` from Termux's own repo, translated):
  a different, also-fatal problem -- Termux's `.deb`s bake **absolute
  Termux paths directly into the archive** (`./data/data/com.termux/...`,
  not Debian's relative `./usr/bin/...`), so `dpkg --instdir=$DN` installs
  them at `$DN/data/data/com.termux/...` instead of `$DN/usr/bin/...` --
  a packaging-format incompatibility, not fixable by translation. Termux's
  apt/dpkg control metadata also declares `Depends`/`Conflicts` in
  Termux's own package names (`libc++`, `zlib`, ...), unresolvable against
  a Debian package index regardless of the path issue.
- **sudo-less** (a sibling project, `jronminh/sudo-less`) already solved
  this properly: forks Termux's same apt/dpkg patches, cut down to what a
  plain unprivileged prefix needs (not Android-specific), rebased onto
  current Debian apt/dpkg source (`apt-dpkg/patches/`, `UPSTREAM.md` has
  the full per-patch rationale). Real fix, if this is picked up again, is
  building from Debian's source with (an adapted version of) that patch
  series -- same shape of effort as 0.5.0's glibc patch, not attempted
  this session given the time already spent. Reverted cleanly: `apt`/
  `dpkg` are back to `dn-standins.sh`'s stand-in, unchanged in
  architecture from before this attempt (the `$DN/usr/bin` priv/PATH fix
  and `DN_REDIRECT_DEBUG`'s fuller dump, both found productive along the
  way, were kept).

**Open**:
- Prefix location: keep `~/.dn` (a `$DN/root` -> `~` symlink loop is
  harmless for `find`/`du`, not for `-L`), or move it out of `$HOME`.
- `dpkg-trigger` under `DPKG_ROOT`: check a trigger-using package; wrap
  like `dpkg-divert` if it double-prefixes.
- `dpkg --print-architecture` answers `aarch64` inside the prefix — watch
  for a maintainer script expecting `arm64`.

**Next release (not 0.2.0): the repo.** The same translation at repo
build time in [`deb-native-repo`](https://github.com/jronminh/deb-native-repo)
(private) — packages arrive pre-translated and signed; device hooks stay
as a fallback.

## 0.3.0: fake root (released, no further investment planned)

**Goal**: inside the prefix a program sees itself as root, as on a Debian
where apt/dpkg/maintainer scripts run as root. Only the identity is
faked; no right is gained, nothing is recorded.

**Status**: released. The shim (`native/path-redirect.c`) fakes
`get[e]uid`/`get[e]gid`/`getres[ug]id`/`getgroups` -> 0, `stat` ownership,
no-ops `chown`/`set*id`/`setgroups`/`initgroups`, and `USER`/`LOGNAME` in
the environ array; `dn-trace` does the same at syscall exit for static
programs/raw syscalls/NSS; `DN_ID=user` opts a command and its children
out (`postgres`, Chromium's sandbox).

**Reconsidered 2026-10-01: no further investment in fake-root itself.**
It stays exactly as released — still needed today, maintainer scripts and
plenty of packages assume root — but this is a stand-in, not a
destination: the real fix is the "Services, then sudo" layer below (a
real extra identity via `termux-adb-bridge`/`dsb`, not a faked one). Given
that, sinking more effort into fake-root's own mechanism (tracer-cost
optimization, the SECCOMP_RET_USER_NOTIF prototype) isn't worth it —
better spent once on the real identity layer than twice. The parked
`set-fakesyscalls-parked.patch` (0.5.0, `setuid`/`setgid`/...) stays
unapplied for the same reason: nothing about fake-root is being
extended, so there is no new decision forcing it in. The tracer-cost
discussion below is kept for the record, not as planned work.

**Open**:
- Survey on the fake-root prefix (maintainer scripts now run "as root") —
  the one item still worth doing, since it's measuring the *existing*
  mechanism, not building on it.

**Tracer-cost discussion (2026-09-30, record only — not planned work per
the note above)**:
1. `SECCOMP_RET_USER_NOTIF` instead of `ptrace` for `dn-trace` — flagged
   as "the endgame" in `docs/spec/direct-usage.md`/`docs/spec/syscall-boundary.md`/
   `docs/spec/shim-coverage.md`, never attempted. Can allow/deny/inject an
   fd/return a value, but cannot rewrite a syscall's arguments in place
   the way `ptrace` can (path rewriting, the tracer's main job, would
   need `process_vm_writev`) — needs a small prototype against
   `dn-trace`'s rewrite paths (`path/path.c`) before committing.
2. Narrow what still falls through to the tracer, rather than speeding it
   up. Widening the *shim* to catch NSS was tried and closed negative
   (`docs/spec/shim-coverage.md`, `docs/spec/syscall-boundary.md`) — glibc's NSS
   opens through a private, link-time-bound symbol no `LD_PRELOAD`
   reaches. 0.5.0's own-glibc is the real fix (below). Cheaper interim,
   not started: narrow `native/dn-run.c`'s `classify()`/`has_nss_import()`
   (routes a whole process to the tracer for its entire lifetime just for
   *importing* an NSS symbol, regardless of whether it's called) — measure
   the false-positive rate on the survey sample first. Newly relevant, not
   just theoretical: `classify()` had its own bug until the runtime
   component audit (below) fixed it 2026-09-30 — it was misrouting
   *every* prefix glibc binary to a bare, untraced `execv()` regardless of
   NSS imports, so this "too broad" concern was moot until then (nothing
   was actually being routed). It is a live cost now.

## 0.4.0: a lighter base (deferred until after alpha)

**Goal**: a lighter bootstrap — not a package swap, a different kind of
Debian base (as in Debian's own installer environment).

**Status**: deferred 2026-09-30 — large, high-risk (touches what nearly
everything else in the prefix depends on), not required for alpha.
Leaning, per the findings below: **prebuilt base + apt-cache cleaning
(+ parallel translation, already released) first; a busybox base only if
size still matters after that.** Full options/tradeoffs to be recorded in
`docs/spec/design-0.4.0.md` once written; findings so
far (trixie index, 2026-09-27):
- The kept GNU base (`mawk coreutils sed grep findutils debianutils
  diffutils gzip tar hostname`) is Essential and depended on
  (`base-files` -> `awk`, `dash` Pre-Depends `debianutils`) — a busybox
  swap needs stand-in rules, and 9/10 of these are assumed-GNU by other
  packages (measure the breakage: `sed -z`, `grep -P`, GNU `find`).
  `debianutils` has no busybox equivalent for `add-shell`/`update-shells`.
  Gain is modest (~5MB download) — the real win is probably translation
  time, not measured per-stage yet.
- **Parallel translation released** (`DN_JOBS`, default `nproc`):
  translate 41s -> 14s, fresh install 1m37s -> 1m5s (fe2 baseline below).
- Prebuilt base (build+translate once in CI/`deb-native-repo`, ship as a
  tarball) skips most of translate+configure — likely the largest win,
  close to how Termux itself installs. Fits the default `~/.dn` path
  (paths are embedded); other paths fall back to local bootstrap.
- Cleaning `var/cache/apt` after bootstrap (-85MB) is a trivial win,
  could ship in 0.2.x independent of everything else here.

**Baseline (0.2.0, fe2, 2026-09-27, fresh install)**: 1m37s total
(translate 41s, configure 23s — the two targets); prefix 224MB/2,233
files (85MB cache + 56MB apt lists, ~83MB actual base).

**0.4.0 is done when**, against that baseline and the same survey sample:
fresh install clearly faster (translate+configure, 64s, is the target);
fewer base packages; smaller prefix (cache cleaned + lighter base);
survey installed/working >= 99/98 (no regression); maintainer scripts
(`ca-certificates`, `passwd`, `fastfetch`, a shell package,
`update-alternatives` users) still pass.

**Open**:
- Find what configure's 23s actually is (likely `ca-certificates`
  rebuild, `debconf`) before deciding whether to cut it.

## 0.5.0: our own glibc (in progress)

**Goal**: the prefix's `libc6` stops being an imposter (Termux's glibc
under Debian's name) and becomes Debian's own glibc source, at Debian's
exact version, with Android compatibility patches applied by this
project's own patch pipeline — matching mainstream Debian for real, not
faking it. This is also the real fix for the tracer's NSS case (0.3.0's
"Speed under the tracer" discussion): a glibc built and patched by this
project can read the prefix's `/etc` directly, no tracer route needed.

**Scope, closed 2026-09-30**: own-glibc has no leverage on three gates
below glibc entirely (a syscall failing there fails the same way no
matter which library issued it) — the app seccomp allowlist, capability/
kernel-config gaps, and SELinux (full detail:
[`docs/spec/android-platform.md`](docs/spec/android-platform.md)). Its
confirmed leverage is the NSS/loader-internal-path class (NSS, `gconv`,
locale, `ld.so.cache`, `RUNPATH`) plus whatever syscall stock Debian
`libc6` trips at startup. `io_uring` (real gap, Gate A) is deliberately
out of scope (HPC concern, not this project's); namespaces/mount/
overlayfs/`swapon`/`mknod`/SysV IPC/ports <1024 were triaged out without
needing a device test.

**Status**: the patch (`third_party/glibc-android-patches/dn-glibc-android.patch`,
forked from `termux-pacman/glibc-packages`, retargeted to this project's
fixed prefix) is complete for everything in scope — 148 file-diffs,
round-trip-verified. Validated via a clean-room rebuild (fresh Debian
source + this patch alone, no hand-fixes, `-j8`): builds clean, `make
install` clean, `hello` runs, NSS resolves the prefix's real `/etc`
natively, terminal I/O (`isatty`/`tcgetattr`/`tcsetattr`, baud-rate
round-trip) works correctly with no port needed
(`disable-termios2.patch` turned out unnecessary — `termios2` doesn't
exist anywhere in glibc 2.41's source). Patch catalog and per-file verdict:
[`docs/spec/android-platform.md`](docs/spec/android-platform.md). Full
investigation history:
[`docs/log/android-seccomp-audit.md`](docs/log/android-seccomp-audit.md),
[`docs/log/findings/`](docs/log/findings/README.md). One parked decision: the
fake-root-entangled `"0"`-bucket of `fakesyscall.json`
(`setuid`/`setgid`/...) is split out as
[`set-fakesyscalls-parked.patch`](third_party/glibc-android-patches/set-fakesyscalls-parked.patch),
unapplied — stays that way: fake-root's future is decided (0.3.0,
reconsidered 2026-10-01: no further investment, superseded eventually by
the real identity layer), and the decision is *not* to extend fake-root,
so nothing calls for applying this patch. One known non-blocking bug:
`ldconfig -r` `SIGSYS`s when run
untraced, succeeds under `dn-trace` (`ld.so.cache` isn't required for the
loader to work, not investigated further).

**`libc6` packaged and installed, 2026-10-01**: the validated patch is
now the prefix's actual, running `libc6` — `dpkg -l libc6` shows `ii
2.41-12+deb13u4`, replacing `dn-standins.sh`'s Termux-glibc stand-in.
[`scripts/bootstrap/dn-package-glibc.sh`](scripts/bootstrap/dn-package-glibc.sh) builds it:
real Debian `libc6.deb` as a template (its maintainer
scripts/triggers/symbols/doc are still accurate, reused as-is), payload
replaced with this project's own build, relocated from the build's flat
`--disable-multi-arch` layout into Debian's real multiarch directory
(confirmed safe -- no binary has that path baked in as a literal
string). Recipe, including the one file `make install` doesn't produce
(`gconv-modules.cache`, generated via the build's own `iconvconfig`), is
in [`third_party/glibc-android-patches/README.md`](third_party/glibc-android-patches/README.md).
Verified: NSS resolves real identities (`ls -l`), previously-installed
packages keep running, the runtime-component-audit's regression battery
(`find -exec test`, a fresh `apt-get install`) stays clean installing
over the live prefix, not just in the scratch build dir.

**`libc6-dev`/`libc-dev-bin` need no packaging pass at all, 2026-10-01**:
first tried packaging them too (same template-and-replace approach as
`libc6`), but that only pushes the exact-version-match wall one package
down the dependency graph indefinitely -- a patch chain. The actual fix:
`dn-package-glibc.sh` keeps the custom `libc6` build's version string an
*exact* match to Debian's (no `+dn1` suffix, removed), since it's the
same upstream source and Debian patch series plus one Android
compatibility patch on top, not a different thing wearing the name. With
that, `libc6-dev`/`libc-dev-bin`'s `Depends: libc6 (= ...)` sees a true
match and both install straight from Debian's real archive, unmodified
(`docs/log/findings/libc6-dev-gap-closed.md`). `locales`/`libc-bin` likely work the same way
but untested -- see Open below.

**Considered, not now: split the glibc patch/build/packaging into its own
repo** (2026-10-01), to become a real standalone Termux-glibc fork rather
than living under `third_party/` here. Deliberately deferred: the patch
currently retargets a hardcoded `deb-native`-specific prefix path
(`/data/data/com.termux/files/home/.dn`, `set-dirs.patch`'s whole point),
not a generic one, so the main benefit of splitting -- reuse by other
projects -- doesn't exist yet; there is exactly one consumer. 0.5.0 also
isn't done (`libc-bin`/`locales` still pinned and untested, no CI
pipeline) -- splitting mid-design would mean syncing two repos through
changes that are still settling. Revisit once either that's settled plus
a CI pipeline are in place, or a second real consumer wants this -- at
that point, splitting is also the natural moment to generalize the
hardcoded path instead of carrying it over as-is.

**Open**:
- **`libc-bin` / fused-loader migration**: packaging done 2026-10-03 --
  `scripts/bootstrap/dn-package-libc-bin.sh` repackages this build's own
  `ldconfig`/`ldd`/`getconf`/... as a real `libc-bin` `.deb`. It is
  *path*-sensitive (`ldconfig` writes the prefix's `ld.so.cache`), so
  unlike `libc6-dev` Debian's real one cannot be reused
  (`docs/log/findings/own-glibc-missing-libc-bin.md`); it stays pinned in
  `setup-apt-prefix.sh`, deliberately, like `libc6`. **The live `.dn` is
  migrated to the fused loader** (`docs/spec/dn-glibc-prefix.md`):
  `ld.so.preload`/`ld.so.conf` wired, cache built at
  `<prefix>/usr/etc/ld.so.cache`, and 189 ELFs repointed `ld-dn` ->
  `usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`. One loader patch was
  needed after all: `elf/rtld.c` ignores the inherited `LD_PRELOAD`
  (`dn-glibc-android.patch`) because `ld-dn` used to sanitize the env and a
  host preload (Termux's termux-exec) aborts a fused program;
  `native/dn-run.c` updated to match. Still open: register `libc-bin` with
  `dpkg` (currently unpacked by hand) and retire `ld-dn` once verified.
- **`libc-l10n`/`locales`**: still pinned to -1 in
  `setup-apt-prefix.sh`; the same exact-version-match reasoning as
  `libc6-dev` likely lets them install unmodified (not yet tested).
- **Build pipeline**: cross-build in CI (too slow on-device for a real
  release cadence) instead of the on-device build this used -- needed
  for a repeatable release process, not for this validation.
- **Publishing**: `deb-native-repo`, shared with 0.4.0's prebuilt base —
  one pipeline for both.
- **`dn-trace` upgrade** (bundled into 0.5.0 on purpose, not a separate
  release — after own-glibc removes the NSS case, the tracer's permanent
  job stays exactly static binaries + raw `syscall()`):
  1. **Clean death instead of a kill** (Gate A only): a syscall absent
     from Android's seccomp allowlist `SIGSYS`-kills the whole process
     instead of returning `ENOSYS`; `tracer/tracee/seccomp.c` already
     does this for `set_robust_list` — extend it to other Gate-A
     syscalls (starting with `io_uring_setup`/`_enter`/`_register`).
     Needs `dn-run.c`'s `classify()` extended too (an `io_uring`-linked
     dynamic glibc binary runs shim-only today, no tracer attached).
  2. **Speed (`SECCOMP_RET_USER_NOTIF`)** — reconsidered 2026-09-30, not
     committed for this version: real implementation weight for a
     performance gain, not correctness; revisit only after (1) ships and
     only if `dn-trace` is still small.

## 0.5.2: a config-driven loader (released)

**Goal**: `native/ld-dn.c`'s policy stops being C literals -- extending it
(preloads, library search dirs, extra env, redirect roots, per-program
overrides) is a config edit in the prefix, no rebuild.

**Status**: released 2026-10-02. The loader reads
`$DN/etc/deb-native/ld-dn.conf` (shipped as `native/ld-dn.conf`), and
compiled defaults reproduce the pre-0.5.2 environment when the file is
absent, so a fresh prefix still bootstraps. `setup-runtime.sh` installs
the file once (a prefix's own edits survive a reinstall) and replaces
binaries atomically. `native/path-redirect.c` consumes
`DN_REDIRECT_PREFIXES`, so `ld-dn.conf`'s `shim-prefix` changes the shim's
redirect roots with no rebuild. `tests/ld-dn-config/run.sh` covers
defaults, the default file, overrides, per-program blocks and fail-open.
Design and as-built detail:
[`docs/log/ld-dn-config.md`](docs/log/ld-dn-config.md).

**Open**:
- Benchmark the always-probe `openat` per exec on a spawn-heavy workload;
  add the `DN_CONFIG`-gated fast path only if it shows.
- Point `dn-trace`/`dn-run` at the same policy file (loader + shim only
  today).
- Globbing in `[program]` names, if a real case needs it.

## 0.5.3: honour the caller's `LD_LIBRARY_PATH` (released)

**Goal**: adding a project's own library directory follows the standard
glibc mechanism, not a project-specific escape hatch -- and there is
exactly one documented way to do it.

**Status**: released 2026-10-02. `native/ld-dn.c` merges the caller's
`LD_LIBRARY_PATH` after the two fixed prefix dirs, deduplicated, instead
of discarding it; `DN_EXTRA_LIB_PATH` is removed. Both `docs/guides/`
guides now document only `LD_LIBRARY_PATH`
([`docs/guides/gcc-glibc-dev.md`](docs/guides/gcc-glibc-dev.md),
[`docs/guides/python-venv.md`](docs/guides/python-venv.md)).
`tests/ld-dn-config/run.sh` covers the merge and deduplication, and an
on-prefix `gcc` shared-library build runs with `LD_LIBRARY_PATH=.` and
fails without it. Design:
[`docs/log/ld-dn-config.md`](docs/log/ld-dn-config.md).

**Open**: none.

## 0.5.4: fail-open hardening (released)

**Goal**: a config mistake warns and is skipped, never takes the whole
prefix down; and the config file, not a compiled default, is the source
for an installed prefix.

**Status**: released 2026-10-02. `native/ld-dn.c` no longer `die`s on a
value too long for its cell or a full table -- `env`, `lib-add`,
`preload`, `loader` and `shim-prefix` overflows now warn and skip, as the
"fail open" design always claimed (previously one oversized `env` value
or `lib-add` path killed every prefix program with exit 127).
`native/ld-dn.conf` sets `shim-prefix` active, so the redirect roots come
from the config rather than the shim's compiled switch, and it documents
that global directives must precede the first `[program]` block.
`tests/ld-dn-config/run.sh` covers oversized values, an oversized file
and aggregate overflow. Design:
[`docs/log/ld-dn-config.md`](docs/log/ld-dn-config.md).

**Open**:
- Gate/benchmark the always-probe `openat` (see 0.5.2 Open).

## Services, then sudo (after alpha)

**Goal**: same scope as `sudo-less` — a service needs something to run it
and the rights it expects. In order: services first, then sudo modes,
then both together.

**Status**: not started. `sudo`/`doas` are pinned to -1 in the prefix
(setuid-root binaries that cannot work here; the names are reserved for
this project's own). The no-op `chown`/`chgrp`/`dpkg-statoverride` (and
`getent`/`update-rc.d`/`invoke-rc.d`/`deb-systemd-helper`/
`deb-systemd-invoke`) are already grouped into one privilege layer
(`$DN/usr/lib/deb-native/priv/`) a later real mode can replace.

**Design for services, decided**: real systemd is out (PID 1, or
`--user` with cgroups/a session bus Android gives an app none) —
mimicking all of it is a trap. Instead: packages keep shipping units and
calling `deb-systemd-helper`/`systemctl`; deb-native translates each unit
at install (`ExecStart` foreground as the app user,
`Environment`/`EnvironmentFile`, `WorkingDirectory`,
`RuntimeDirectory`/`StateDirectory` -> `$DN/run|var/lib/NAME`; `User=`
and sandbox options dropped; low ports/capabilities/devices refused with
the reason); a small `systemctl` front maps
`start/stop/restart/status/enable/disable` to `sv` (termux-services/
runit), anything else says "not supported". Rule: translate what maps to
a supervised process, refuse the rest. Test ladder: `cron` -> `redis`
(system user + data dir) -> `dbus` (a socket in `/run`). Android may kill
background services (phantom-process killer, battery optimization) —
document the wake-lock/battery settings needed (see `scripts/bench/perf-run.sh`
for the mechanism).

**`sudo`, three stackable kinds, never Android root**: pass-through
(installers that just prefix `sudo`), fake root (uid 0 believed,
ownership recorded — 0.3.0's mechanism), a real extra identity (Android's
shell uid via `termux-adb-bridge`, or a bounded identity as in `dsb`).

**Open**:
- `update-rc.d`/`invoke-rc.d`/`deb-systemd-helper` -> real runit
  translation (needs `/run` in the shim, system users in the prefix's
  database — overlaps with sudo).
- Keep `dn-run`/`dn-shell` preload handling general enough for a second
  `LD_PRELOAD` (a fake-root library beside the shim, for a stronger sudo
  mode later).
- Keep `base-passwd`'s `root` user and `sudo` group as Debian has them.

## Shim & tracer hardening

**Goal**: every path-taking libc call a glibc program can make is
correctly intercepted and rewritten into the prefix; what the shim
structurally cannot reach (static binaries, raw `syscall()`, libc-internal
opens) is caught by the tracer instead.

**Status**: the libc-interposition layer is complete for its scope —
[`docs/spec/shim-coverage.md`](docs/spec/shim-coverage.md)'s 258-package in-scope
corpus has every imported path-taking symbol intercepted (full family:
`open`/`stat`/`exec`/`spawn`/`xattr`/`mkfifo`/`mknod`/AF_UNIX/
`inotify`/`mkstemp`/... — see `tests/shim-libc/run.sh`), except NSS
(confirmed structurally unreachable at this layer, 0.5.0 fixes it),
`glob`/`glob64` (indirect), admin ops, and the raw-`syscall()` boundary
(the tracer's permanent job). The tracer (`dn-trace`, replacing PRoot)
is built at setup, arm64-only, with the direct-syscall attribute
(`scan-direct-syscalls.py --trace-list`) routing static/raw-syscall
binaries to it automatically; Termux's `proot` fallback is dropped
entirely (0.2.3) — without `dn-trace`, `dn-run` warns and runs
untranslated. Tracked in
[GitHub issue #1](https://github.com/jronminh/deb-native/issues/1).

**Fixed 2026-09-30 (runtime component audit, below, has the full
writeup)**: a real, root cause bug — `path-redirect.c`'s
`target_is_glibc()` checked a target's `PT_INTERP` for `"ld-linux"`/
`"/glibc/"`, which never matches this project's own translated binaries
(rewritten to `ld-dn` by `dn-translate-deb.sh`) — misclassifying nearly
every prefix binary as Bionic, which could reorder its `PATH` and leak
it into the wrong process, segfaulting Android's linker (`find -exec
test`, `env test`, apt's own install pipeline all hit this). Fixed by
recognizing `ld-dn` too; a second, related gap (`bionic_env()` stripped
`LD_PRELOAD` for a Bionic child but not `LD_LIBRARY_PATH`/
`COMPILER_PATH`, confirmed crashing Termux's own `dpkg-deb`) fixed the
same way. Verified: `find -exec test`, a full `apt-get install` of
several previously-uninstalled packages, and the `ca-certificates`
postinst all complete with zero segfaults and zero `logcat` crash
entries.

**Open**:
- Bake the shim into installed ELFs (`docs/spec/path-shim.md`, "Delivering
  the shim") so it survives an empty environment — `patchelf --add-needed`/
  `--add-rpath` or a `DT_AUDIT` module; the explicit loader
  (`ld.so --preload`) is the simpler variant.
- Test `dn-run` -> `dn-trace` from an installed prefix (not just the dev
  checkout).
- ~~`setup-runtime.sh`'s later build steps segfaulted/bus-errored
  transiently twice~~ — likely resolved as a side effect of the fixes
  above: 10/10 clean runs afterward (forcing a full native rebuild each
  time), zero `logcat` crashes. Not proven (never caught a crash under
  the pre-fix shim specifically to confirm the mechanism), so watch for
  recurrence rather than close outright.
- **Not scheduled: route a "well-behaved" binary to the tracer when it
  needs it for a reason today's scans can't see.** Found via `ldconfig
  -r` (0.5.0 status, above): it imports no NSS symbol and emits no raw
  syscall of its own (so neither `classify()`'s NSS scan nor
  `scan-direct-syscalls.py`'s syscall/`svc` scan flags it) — it dies
  calling an ordinary public libc function whose *internal*
  implementation probes a newer syscall and falls back on `ENOSYS`, a
  graceful pattern on a real kernel that instead gets Android's seccomp
  filter delivering a fatal `SIGSYS` (no `ENOSYS` to fall back from).
  Nothing in the ELF says which public libc calls can do this on this
  device's kernel/glibc build — it isn't a property of the package at
  all, so per-package static scanning structurally can't catch it.
  Discussed 2026-09-30/10-01, not attempted, three shapes on the table:
  (a) route everything through the tracer — rejected, defeats the
  shim's whole reason to exist; (b) reactive: run untraced, detect a
  child killed specifically by `SIGSYS` (not any failure), re-exec the
  same argv under `dn-trace`, and cache the verdict (path+mtime) so it
  routes straight to the tracer next time — same pattern
  `survey-prefix.sh`'s `try_program()` already uses ad hoc for
  measurement, just never promoted to a real runtime mechanism; (c) a
  one-time audit of glibc's own source (per glibc build, not per
  package) against `docs/spec/android-platform.md`'s allowlist, to name
  the exact handful of public libc functions with a probe-and-fallback
  syscall pattern, then watch only those in the shim — more precise
  than (b), more upfront cost, pays off once instead of per-crash.
  User's framing: this should be a **post-install/post-adopt analysis
  pass** (extending what `scan-direct-syscalls.py` already does once per
  package at install time, generalized past raw-syscall detection),
  not a per-launch runtime check — closer to (c)'s shape than (b)'s.
  The routing/wiring decision itself is still explicitly next-plan, not
  now; but the analysis pass needs visibility data to design against,
  which didn't exist before, so that groundwork was done 2026-10-01:
  `tracer/tracee/seccomp.c`'s SIGSYS `default` case (already a generic
  catch-all for any blocked syscall, known or not) now `note()`s the
  syscall's name and raw number before returning `ENOSYS`, visible at
  default verbosity. Confirmed against `busybox-static`'s `true`
  (already installed): every traced glibc/NPTL program hits `rseq`
  (glibc >= 2.35 registers it for every thread, including the main one,
  at startup) — previously silently swallowed as unnamed ("void",
  syscall #293 had no entry in `sysnums-arm64.h` at all), now named.
  Already handled correctly before this (clean `ENOSYS`, glibc's own
  registration tolerates it) — this only adds visibility, no behavior
  change. Next step for the analysis pass itself: run this logging
  against a real package's actual binaries (not just a synthetic probe)
  to gather data before choosing (b) vs (c).

## Runtime component audit (debt from rapid early development)

**Goal**: the runtime support components (`native/path-redirect.c` the
shim, `native/ld-dn.c` the loader stub, `native/dn-launch.c`/`dn-run.c`,
the translate-time scripts that wire them together) were built fast,
iteratively, patch-by-patch as each new failure surfaced (`docs/log/findings/`
is the record of that) -- not from a single coherent design pass. That's a
reasonable way to get here, but it means logic in these files hasn't had
a systematic re-read since; bugs can hide in interactions between pieces
that were each individually reasoned-through in isolation, at different
times, under different assumptions. Go back through each one deliberately,
not just reactively when something crashes.

**Status**: first full pass done, 2026-09-30, all five items closed out.
Diagnostic-first approach worked where reading-the-source-alone hadn't:
temporary debug instrumentation in `path-redirect.c` (kept, gated on the
existing `DN_REDIRECT_DEBUG` env var, zero cost when unset) found the
actual mechanism behind the `find -exec test`/`env test` segfault in one
run, after source-reading alone had produced a confident, wrong
prediction. Three real bugs found and fixed, all one root pattern:

- **`path-redirect.c`'s `target_is_glibc()`** checked `PT_INTERP` for
  `"ld-linux"`/`"/glibc/"` — never matches this project's own translated
  binaries (`ld-dn`), so it misclassified nearly every prefix glibc
  program as Bionic. Consequence: `bionic_env()`'s `PATH`-reordering
  (meant for a genuine Bionic child) got applied to glibc programs too,
  pointing their own command lookups at Termux's binaries instead of the
  prefix's — which then carried a leaked glibc `LD_PRELOAD`, segfaulting
  Android's linker.
- **`bionic_env()`** stripped `LD_PRELOAD` for a Bionic child but never
  `LD_LIBRARY_PATH`/`COMPILER_PATH` (both set by `ld-dn` for its glibc
  target) — confirmed reproducing Termux's own `dpkg-deb` failing to
  link when launched with the prefix's `LD_LIBRARY_PATH` still set.
- **`dn-run.c`'s `classify()`** had the exact same `"ld-linux"`-only bug
  independently (found by checking for the same pattern after fixing it
  in the shim, not independently) — every prefix glibc binary fell
  through to `C_DYNOTHER`'s bare `execv()`, silently skipping the NSS
  tracer-routing `dn-run` exists for. More serious than the "too broad"
  concern the 0.3.0 section flagged — it was the opposite, not routing
  *at all* for this project's own binaries. `id` (imports
  `getpwuid`/`getgrgid`) now correctly routes to the tracer and resolves
  the fake-root identity.

Two items reviewed and found sound, no code change: `posix_spawn()` was
missing the script/shebang branch `do_exec()` (execve's dispatcher) has
— fixed for consistency, script targets now get the same treatment
everywhere. `map_shebang_interp()` (the shim's own runtime shebang
mapping) still hardcoded `dn-shell` unconditionally after the
translate-time scripts were fixed to prefer the prefix's own dash/bash —
fixed to match. `ld-dn.c`'s stack-rebuild math checked against a
realistic large-argv/envp case (a multi-package `dpkg` invocation) —
`words`'s bound is ~8000 words of headroom against realistic usage in
the low hundreds, comfortably safe. `dn-launch.c`'s remaining call sites
(`grep -rn dn-shell`) are either the confirmed bootstrap fallback or
legacy/retired scripts (`patch-maintainer-scripts.sh`, only reachable via
the inactive `bootstrap-base.sh`/`prototype-install.sh`/`survey.sh`
pipeline) — left alone, out of scope.

Verified end to end after each fix: `find -exec test`, `env test`, a
full `apt-get install` of several previously-uninstalled packages
(`tree`, `cowsay` pulling in `perl`, `figlet`, `sl`), and the
`ca-certificates` postinst all complete with zero segfaults and zero
`logcat -b crash` entries.

**Open**: none from this pass. New, smaller item found along the way:
`setup-runtime.sh`'s build steps segfaulted/bus-errored transiently
twice (always clean on retry) — noted under "Shim & tracer hardening",
not chased down.

## Runtime overhaul

**Goal**: the runtime pieces were named and built as the design evolved, and
some names now misdescribe what the code actually does. `native/ld-dn.c` is
the clearest case: the name reads as "deb-native's ld-linux" (a loader), but
it is an **interpreter trampoline** in the `PT_INTERP` slot -- it prepares
the environment and does the kernel-side handoff (rebuilds
`argc`/`argv`/`envp`/`auxv`, maps glibc's real `ld-linux-aarch64.so.1`,
sets `AT_BASE`, jumps to its entry), then steps aside; the actual dynamic
linking is still done by glibc's loader. Rename it **`dn-interp`** (the name
that matches the slot it fills) and re-read the rest of the runtime for the
same class of misnomer.

**Status**: decided 2026-10-03 (rename `ld-dn` -> `dn-interp`), not started.
`native/README.md` and `ld-dn.c`'s own header call it a "program loader
stub", the same ambiguity in prose. The `patchelf` replacement now has its
exact spec written down -- `docs/spec/elf-interp-patch.md`'s `dn-elf`
section (`get-interp`/`set-interp`, the two `PT_INTERP` fields touched, the
append-when-longer rule); the tool itself is not built yet.

**Open**:

- Rename `ld-dn` -> `dn-interp` across the tree (~40 files: `native/ld-dn.c`,
  `native/ld-dn.conf`, `setup-runtime.sh`, `dn-translate-deb.sh`,
  `dn-adopt.sh`, `make-launchers.sh`, `dn-fix-gcc-specs.sh`,
  `tests/ld-dn-config/`, docs). The install path is baked into every
  translated ELF's `PT_INTERP` and self-matched in `ld-dn.c` (the
  `/usr/lib/deb-native/ld-dn` suffix it strips to find the prefix), so keep
  a compat symlink `ld-dn -> dn-interp` or force a re-bootstrap (pre-alpha,
  so a re-bootstrap is acceptable).
- Same pass: any other runtime name that no longer describes its mechanism
  (`dn-run`'s "launch classifier", "loader stub", ...); fold findings into
  the "Runtime component audit" above.
- Build the self-brewed replacement (`dn-elf`) per
  `docs/spec/elf-interp-patch.md`: read/set the one `PT_INTERP` field in
  `dn-translate-deb.sh` (its only call site), dropping `patchelf` and its
  failure modes.
- Fuse `ld-dn` into the loader (**`dn-glibc`**): the kernel loads the
  patched `ld-linux` directly and the trampoline disappears -- plan,
  pros/cons, and the trigger in `docs/log/ld-dn-runtime.md`'s
  "Alternative: fuse into the loader". Name locked 2026-10-03; develop and
  validate the hook on amd64 first, arm64 on-device after.

## Quick wins

- [x] ~~Forked Bionic `sed`/`find` in some postinsts can't see prefix
  paths~~ — fixed 2026-09-30: the `dpkg`/`apt` wrapper scripts now put
  `$DN/usr/bin` ahead of `PATH` for their child processes (maintainer
  scripts), without touching the interactive shell's `PATH`. Verified
  against the real `ca-certificates` postinst.
- [x] ~~`apt install`-driven maintainer scripts can't see `$DN/usr/bin`
  (only a direct `dpkg -i` through the launcher wrapper could)~~ — fixed
  2026-10-01: `apt` forks dpkg via its own compiled-in `Dir::Bin::dpkg`,
  bypassing the project's dpkg wrapper entirely; added `DPkg::Path` to
  the prefix's `apt.conf` (`setup-apt-prefix.sh`). Found installing
  `gcc`'s dependency `cpp` (`dpkg-maintscript-helper: not found`),
  `docs/log/findings/apt-install-gcc-end-to-end.md`.
- [x] ~~`libc6`/`dpkg`/`apt` stand-ins were never `dpkg hold`-ed (only
  the `$BASE` set was)~~ — fixed 2026-10-01: `dn-standins.sh`'s shared
  `install_pkg()` now holds every stand-in it installs. Without it, a
  real Debian `libc6` pulled in by `apt upgrade` would have replaced the
  working one with a build that segfaults at startup
  (`docs/log/android-seccomp-audit.md`). Same reasoning noted in
  `third_party/glibc-android-patches/README.md`'s manual recipe for when
  the real own-glibc build replaces the stand-in.
- [x] ~~`path-redirect.c`'s shim only redirected `/usr`, `/etc`, `/var`,
  `/opt`, `/root` -- `ld` couldn't find `/lib/aarch64-linux-gnu/libc.so.6`
  linking a plain `gcc -o hello hello.c`~~ — fixed 2026-10-01: added
  `/lib`, `/bin`, `/sbin` as three more redirected prefixes (Debian's own
  merged-usr aliases for `/usr/{lib,bin,sbin}`, same symlinks the
  prefix's `base-files` already sets up) — `docs/spec/shim-coverage.md`,
  `docs/log/findings/gcc-hello-pt-interp-gap.md`.
- [x] ~~A `gcc`-linked binary's `PT_INTERP` is a literal, unresolvable
  `/lib/ld-linux-aarch64.so.1` -- fails `cannot execute: required file not
  found` at the kernel level, before the shim ever runs~~ — fixed
  2026-10-01: `scripts/install/dn-fix-gcc-specs.sh` (wired into
  `dn-hook-post.sh`) writes a `specs` file next to each installed gcc
  version's `libgcc.a`, overriding just the `-dynamic-linker` string to
  `ld-dn`'s real path -- GCC's own site-customization hook (same mechanism
  musl/NDK toolchains use), no gcc/binutils patch or rebuild. Idempotent,
  no-op if gcc or `ld-dn` isn't present yet. `gcc -o hello hello.c &&
  ./hello` now compiles and runs end to end.
- [ ] Refresh `README.md`'s status numbers once the unfiltered survey
  (Alpha goal, above) reports.

## Backlog (not yet scheduled)

Ideas with a design sketch but no committed slot — pick up after the
Alpha goal and the sections above. Detail in the linked docs, not here.

- **Stale `apt.conf` hook paths after a repo move** — `setup-apt-prefix.sh`
  writes the prefix's `apt.conf` once, at bootstrap, with absolute paths
  into the checkout as it was that day; nothing regenerates it for an
  existing prefix (`install.sh`'s "refresh" branch never calls
  `setup-apt-prefix.sh` again). Any later script move/rename (this
  session's `scripts/` reorg, for one) breaks `apt`/`dpkg` for anyone with
  an already-bootstrapped prefix
  (`docs/log/findings/apt-install-gcc-end-to-end.md`).
  Candidates: `termux-dn-doctor --fix` detects and rewrites the hook
  lines; or the "refresh" path rewrites just those lines of an existing
  `apt.conf` instead of leaving it untouched.
- **Run Tailscale natively** — the target case for the tracer (a static
  Go daemon the shim cannot see). [`docs/guides/tailscale.md`](docs/guides/tailscale.md).
- **Run wrappers** (`prefix-wrap` equivalent, `docs/spec/classic-design.md`) — the
  biggest unbuilt piece: for each binary a package puts on `PATH`, detect
  whether it needs path help and generate a wrapper, triggered via apt's
  `DPkg::Post-Invoke`.
- **Classifier/refusal** (`prefix-check` equivalent) — read each `.deb`
  before dpkg runs, classify scope and mechanism, refuse a "never"
  package before dpkg can wedge the prefix.
- **Soname-based dependency matching** — a possible future mechanism:
  match a `.deb`'s `Depends:` against installed `*-glibc` packages'
  SONAMEs directly, instead of a hand-written name table. Not a revival of
  `native-seed.sh` (gone, replaced by the real `libc6` stand-in) — see
  `docs/spec/native-reuse.md`, "Where this idea goes next", for the gap
  this would close if ever built.
- **Launcher/icon/desktop-DB integration** — mostly N/A on Android; do
  only what Termux needs.
- **State + `explain` + `doctor`** — record per package its scope,
  mechanism and wrappers, so decisions are explainable/removable.

## Known unsafe, not yet fixed

- `--force-architecture` workaround for the archive-name mismatch
  (`arm64` vs `aarch64`) — flagged unsafe in
  `docs/log/findings/first-working-prototype.md`, needs a
  real fix.

## Blocked / impossible on this device

Kernel-wide, probed 2026-09-26 (`docs/spec/android-platform.md`, "Device
probe: sandbox limits confirmed directly"): user namespaces off entirely (`CLONE_NEWUSER` = `EINVAL` even
seccomp-free), mount namespaces need `CAP_SYS_ADMIN`, `/dev/fuse` is
root-only. Keep these out of scope:

- install-view / service-view isolation (hidden `$HOME`, empty `/run`);
- `prefix-sandbox`'s seccomp + namespace isolation;
- other-architecture (i386) loaders and `Multi-Arch` skew;
- setuid/setgid and file capabilities;
- a TUN device, firewall rules, raw sockets.
