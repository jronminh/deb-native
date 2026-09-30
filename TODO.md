# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/design.md`, "Order
of work"), judged by its criterion: **per-section coverage from a random
sample**, not feature count. See [`docs/vs-sudo-less.md`](docs/vs-sudo-less.md)
for the side-by-side diff.

**Format**: each section below has a **Goal** (stable, rarely changes), a
**Status** paragraph (the current, stable truth — when an Open item is
finished, its outcome is folded in here and the item is deleted, not kept
as a checked-off line), and an **Open** list (short, no narrative — the
"why"/investigation history lives in [`docs/findings.md`](docs/findings.md)
(chronological engineering log) and the other `docs/*.md` files each
section links to).

## Alpha goal (the next announcement)

**Goal**: pre-alpha means "works for the author"; alpha means "try it, it
mostly works". Reached when every item below is done. Announce with one
number ("N of 100 random Debian packages install and run, no root, no
proot"), the comparison with proot-distro (README), and a `gcc` demo.

**Status**: 0.2.0's foundation and 0.3.0's fake-root are released. 0.5.0's
own-glibc patch is written, forked, and validated (clean-room rebuild,
`hello` + NSS working) but not yet packaged as the prefix's real `libc6` —
that's what "Compilers" below is still waiting on.

**Open**:
- **Compilers**: package 0.5.0's own-glibc as the prefix's real `libc6`
  (see that section); `gcc`/`make`/C hello-world; `ghc`/`rustc` as a bonus.
- **Popular languages**: `python3` + a C-extension package, `perl` + an XS
  module, `ruby`, `nodejs` — incl. the Perl version gap (`dn-perl` still
  uses Termux's 5.42; trixie's `perl` builds modules for 5.40, no fix yet).
- **Unfiltered survey**: the seeded 100 across all 43 sections without the
  size filter — a number for heavy packages, no asterisk
  ([`docs/survey-0.2.0.md`](docs/survey-0.2.0.md) is lightweight only).
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
[`docs/survey-0.2.0.md`](docs/survey-0.2.0.md)), a full prefix delete
leaving Termux untouched. Design decisions (real nested Debian root, no
`usr ->.` flattening, Termux's own apt/dpkg through launchers,
`$DN/root` -> Termux home) are recorded in
[`docs/design-0.2.0.md`](docs/design-0.2.0.md). The hotfix that shipped
after (`priv chroot` for maintainer scripts using
`chroot "$DPKG_ROOT"`, e.g. `dbus`/`ipp-usb`/`avahi-daemon`, which
Android's seccomp otherwise SIGSYS-kills) is a temporary fix, still
standing until the identity/services layer replaces it.

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

## 0.3.0: fake root (released)

**Goal**: inside the prefix a program sees itself as root, as on a Debian
where apt/dpkg/maintainer scripts run as root. Only the identity is
faked; no right is gained, nothing is recorded.

**Status**: released. The shim (`native/path-redirect.c`) fakes
`get[e]uid`/`get[e]gid`/`getres[ug]id`/`getgroups` -> 0, `stat` ownership,
no-ops `chown`/`set*id`/`setgroups`/`initgroups`, and `USER`/`LOGNAME` in
the environ array; `dn-trace` does the same at syscall exit for static
programs/raw syscalls/NSS; `DN_ID=user` opts a command and its children
out (`postgres`, Chromium's sandbox).

**Open**:
- Speed under the tracer: faking file owners stops every `stat` at exit
  (`find` over ~1,200 files: 443ms -> 727ms, fe2). If it matters: fake
  only uid/gid in the tracer, or only for paths under the prefix/home.
- Survey on the fake-root prefix (maintainer scripts now run "as root").

**Tracer-cost discussion (2026-09-30, two independent angles, expected to
combine rather than replace each other)**:
1. `SECCOMP_RET_USER_NOTIF` instead of `ptrace` for `dn-trace` — flagged
   as "the endgame" in `docs/direct-usage.md`/`docs/syscall-boundary.md`/
   `docs/shim-coverage.md`, never attempted. Can allow/deny/inject an
   fd/return a value, but cannot rewrite a syscall's arguments in place
   the way `ptrace` can (path rewriting, the tracer's main job, would
   need `process_vm_writev`) — needs a small prototype against
   `dn-trace`'s rewrite paths (`path/path.c`) before committing.
2. Narrow what still falls through to the tracer, rather than speeding it
   up. Widening the *shim* to catch NSS was tried and closed negative
   (`docs/shim-coverage.md`, `docs/syscall-boundary.md`) — glibc's NSS
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
size still matters after that.** Full options/tradeoffs recorded in
[`docs/design-0.4.0.md`](docs/design-0.4.0.md) once written; findings so
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
[`docs/android-seccomp-audit.md`](docs/android-seccomp-audit.md)). Its
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
exist anywhere in glibc 2.41's source). Full history:
[`docs/android-seccomp-audit.md`](docs/android-seccomp-audit.md),
[`docs/findings.md`](docs/findings.md). One parked decision: the
fake-root-entangled `"0"`-bucket of `fakesyscall.json`
(`setuid`/`setgid`/...) is split out as
[`set-fakesyscalls-parked.patch`](third_party/glibc-android-patches/set-fakesyscalls-parked.patch),
unapplied — apply it once fake-root's future (0.2.0/0.3.0 section) is
decided. One known non-blocking bug: `ldconfig -r` `SIGSYS`s when run
untraced, succeeds under `dn-trace` (`ld.so.cache` isn't required for the
loader to work, not investigated further).

**Not yet done**: turning the validated patch into the prefix's actual,
installed `libc6` — it currently only lives in `~/dn-glibc-build/`, a
scratch build directory, not a package.

**Open**:
- **Build pipeline**: cross-build in CI (too slow on-device for a real
  release cadence), producing `libc6`/`libc6-dev`/`libc-bin`/`locales`
  `.deb`s versioned like Debian's (e.g. `2.41-12+deb13u4+dn1`).
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
document the wake-lock/battery settings needed (see `scripts/perf-run.sh`
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
[`docs/shim-coverage.md`](docs/shim-coverage.md)'s 258-package in-scope
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
- Bake the shim into installed ELFs (`docs/design.md`, "Delivering the
  shim") so it survives an empty environment — `patchelf --add-needed`/
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

## Runtime component audit (debt from rapid early development)

**Goal**: the runtime support components (`native/path-redirect.c` the
shim, `native/ld-dn.c` the loader stub, `native/dn-launch.c`/`dn-run.c`,
the translate-time scripts that wire them together) were built fast,
iteratively, patch-by-patch as each new failure surfaced (findings.md is
the record of that) -- not from a single coherent design pass. That's a
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

## Quick wins

- [x] ~~Forked Bionic `sed`/`find` in some postinsts can't see prefix
  paths~~ — fixed 2026-09-30: the `dpkg`/`apt` wrapper scripts now put
  `$DN/usr/bin` ahead of `PATH` for their child processes (maintainer
  scripts), without touching the interactive shell's `PATH`. Verified
  against the real `ca-certificates` postinst.
- [ ] Refresh `README.md`'s status numbers once the unfiltered survey
  (Alpha goal, above) reports.

## Backlog (not yet scheduled)

Ideas with a design sketch but no committed slot — pick up after the
Alpha goal and the sections above. Detail in the linked docs, not here.

- **Run Tailscale natively** — the target case for the tracer (a static
  Go daemon the shim cannot see). [`docs/tailscale.md`](docs/tailscale.md).
- **Run wrappers** (`prefix-wrap` equivalent, `docs/design.md`) — the
  biggest unbuilt piece: for each binary a package puts on `PATH`, detect
  whether it needs path help and generate a wrapper, triggered via apt's
  `DPkg::Post-Invoke`.
- **Classifier/refusal** (`prefix-check` equivalent) — read each `.deb`
  before dpkg runs, classify scope and mechanism, refuse a "never"
  package before dpkg can wedge the prefix.
- **Sync on `apt update`** — keep the prefix's view of Termux's
  `*-glibc` packages fresh so a Termux upgrade doesn't leave stale seeds.
- **Soname-based dependency matching** — replace
  `scripts/native-seed.sh`'s hand-written name table with matching a
  `.deb`'s `Depends:` against installed `*-glibc` packages' SONAMEs
  (`docs/design.md`, "Open work"). Small, direct install-success win.
- **Launcher/icon/desktop-DB integration** — mostly N/A on Android; do
  only what Termux needs.
- **State + `explain` + `doctor`** — record per package its scope,
  mechanism and wrappers, so decisions are explainable/removable.

## Known unsafe, not yet fixed

- `--force-architecture` workaround for the archive-name mismatch
  (`arm64` vs `aarch64`) — flagged unsafe in `docs/findings.md`, needs a
  real fix.

## Blocked / impossible on this device

Kernel-wide, probed 2026-09-26 (`docs/findings.md`, "Platform sandbox
limits"): user namespaces off entirely (`CLONE_NEWUSER` = `EINVAL` even
seccomp-free), mount namespaces need `CAP_SYS_ADMIN`, `/dev/fuse` is
root-only. Keep these out of scope:

- install-view / service-view isolation (hidden `$HOME`, empty `/run`);
- `prefix-sandbox`'s seccomp + namespace isolation;
- other-architecture (i386) loaders and `Multi-Arch` skew;
- setuid/setgid and file capabilities;
- a TUN device, firewall rules, raw sockets.
