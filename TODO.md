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

**After alpha:** services (runit translation), `sudo` modes,
the pre-translated repo, **0.4.0's lighter base** (busybox swap, deferred
2026-09-30 -- not in the Must/Should-have list above, and swapping
`coreutils sed grep findutils debianutils diffutils gzip tar hostname`
for `busybox` touches nearly everything else depends on; too large/risky
a change to sequence ahead of the alpha push), and **true fusion rebuilt
on the 0.2 core** (the `naibed` branch, frozen until alpha: `ld-dn`,
`dn-trace`, the translator and the priv layer, with Termux's prefix as
the root).

## 0.3.0-prealpha: fake root (released)

Inside the prefix a program sees itself as root, as on a Debian where apt,
dpkg and maintainer scripts run as root. Only the identity is faked; no
right is gained, nothing is recorded.

- [x] shim (`native/path-redirect.c`): `get[e]uid`/`get[e]gid`/
      `getres[ug]id`/`getgroups` -> 0; `stat` owner of the real uid/gid ->
      root; `chown`, `set*id`, `setgroups`, `initgroups` refused for lack
      of rights -> succeed; `USER`/`LOGNAME` = root (in the environ array:
      bash reads it directly).
- [x] `dn-trace` (static programs, raw syscalls, NSS): the same at syscall
      exit; Android-trapped `set*id` succeed instead of `ENOSYS`.
- [x] `DN_ID=user` turns it off for one command and its children
      (`postgres`, Chromium's sandbox refuse root).
- [ ] **Speed under the tracer:** faking file owners stops every `stat` at
      exit -- `find` over ~1,200 files 443 ms -> 727 ms (fe2, interleaved
      minimums). The shim's cost is negligible. If it matters: fake only
      the uid/gid calls in the tracer (owners then mismatch in static
      programs), or fake owners only for paths under the prefix / home.
- [ ] Survey on the fake-root prefix (maintainer scripts now run "as root").

**Discussion, not yet written up (2026-09-30, unrecorded verbal session --
reconstructed after the fact, confirm details before acting on them):**
two angles on tracer cost, expected to combine rather than replace each
other:

1. **`SECCOMP_RET_USER_NOTIF` instead of `ptrace`** for `dn-trace` --
   already flagged as "the endgame" in `docs/direct-usage.md` (:16, :102),
   `docs/syscall-boundary.md` (:131) and `docs/shim-coverage.md` (:175),
   but never attempted. `ptrace` stops the tracee twice per syscall
   (entry + exit); a seccomp-bpf filter with `SECCOMP_RET_USER_NOTIF`
   notifies only on the syscalls it lists and answers over one fd via
   `ioctl`, no double stop. Open question carried over from those docs:
   it can allow/deny/inject an fd/return a value, but **cannot rewrite a
   syscall's arguments in place** the way `ptrace` can -- path rewriting
   (the tracer's main job) would need `process_vm_writev` on the tracee's
   existing argument buffer (same uid, so accessible) before
   `SECCOMP_USER_NOTIF_FLAG_CONTINUE`, in-place and no longer than the
   original string. Needs a small prototype against `dn-trace`'s actual
   rewrite paths (`path/path.c`) before committing -- not yet started.
2. **Narrow what still falls through to the tracer at all**, rather than
   speeding the tracer up. First correction: widening the *shim*
   (`native/path-redirect.c`, `LD_PRELOAD` interposition) to catch NSS was
   already tried and closed with a negative result, 2026-09-26 --
   `docs/shim-coverage.md` ("NSS lookups -- confirmed out of the shim's
   reach") and `docs/syscall-boundary.md` ("Solved: NSS, case 2"). glibc's
   `_nss_files_*`/`_nss_dns_*` are defined inside `libc.so.6` itself and
   open through the private `__open_nocancel` (`GLIBC_PRIVATE`), bound at
   link time -- no `LD_PRELOAD` interposer reaches it, and even
   `nsswitch.conf` dispatch is read internally, so a custom NSS module
   cannot be selected either. Do not re-attempt this at the shim layer.
   - **Real fix, already scoped as 0.5.0** ("our own glibc", below):
     building glibc ourselves from Debian's source + our own Android patch
     series means *we* choose the sysconfdir/build config, so NSS reads
     the prefix's `/etc` directly -- no bind trick, no tracer route for
     NSS at all. This closes tracer-only case #2 outright (static binaries
     and raw `syscall()`, cases stay tracer-only regardless -- no libc
     call to interpose on, whoever built the libc). **Reprioritized ahead
     of 0.4.0 per the 2026-09-30 discussion** -- see 0.5.0 section for
     scope/cost (own build pipeline, patch series maintenance).
   - **Cheaper interim, not yet started:** `native/dn-run.c`'s
     `classify()`/`has_nss_import()` (`:67-122`) routes a whole process to
     the tracer for its **entire lifetime** just because it *imports* an
     NSS symbol (`getpwnam`, `getgrgid`, ...) -- common in `ls -l`, `ps`,
     `git log --author`, `bash`'s `~user` expansion -- regardless of
     whether that binary actually calls it this run. Narrowing that check
     (fewer symbols, or distinguishing "just wants the current uid/gid"
     from "needs the prefix's real passwd/group") would cut tracer load
     without touching the shim or waiting on 0.5.0. Not started; measure
     the false-positive rate on `docs/survey-0.2.0`'s sample first.

Angle 1 (`SECCOMP_RET_USER_NOTIF`) and 0.5.0's own-glibc are independent
and expected to combine (one attacks tracer overhead, the other removes a
whole case from needing the tracer). This entry exists so the discussion
isn't lost a second time; each still needs its own write-up once picked
up.

## 0.4.0 roadmap: a lighter base (was 0.3.0; busybox and other options)

**Deferred to after alpha (2026-09-30)** -- not required by the Alpha
goal's Must/Should-have list above, and a large, high-risk change
(swaps out tools nearly everything else in the prefix depends on).
Kept here for the design record; don't pick this up before alpha ships.

Theme: a lighter bootstrap. Not a package swap -- a different kind of
Debian base, as in Debian's own installer environment: Debian's `busybox`
(dynamic, glibc: ld-dn + shim, no tracer; not `busybox-static`) in place
of the GNU tool packages
`mawk coreutils sed grep findutils debianutils diffutils gzip tar hostname`.
Design doc first (`docs/design-0.4.0.md`), then build. Findings so far
(trixie index, 2026-09-27):

- **The kept base depends on them:** `base-files` Depends `awk`, `dash`
  Pre-Depends `debianutils (>= 5.6-0.1)`. Without a stand-in providing
  them, dpkg's database is broken and apt pulls them straight back; with
  one, the real packages may never install. Decide the stand-in rules
  (`Provides:` only? versioned? replaceable by the real package?).
- **9 of the 10 are Essential:** Debian packages do not declare
  dependencies on them and assume GNU behaviour. Measure the breakage
  (GNU-only options: `sed -z`, `grep -P`, GNU `find`), e.g. a survey run
  on the busybox base against the same sample.
- **`debianutils` has no busybox equivalent** for `add-shell`,
  `remove-shell` (every shell package's postinst), `savelog`,
  `update-shells`: keep it, or priv versions.
- **coreutils:** Termux's `coreutils-glibc` is already on maintainer
  scripts' `PATH`; maybe no replacement is needed.
- **Gain:** ~5 MB less to download (`coreutils` 2.9 MB, `tar` 0.8,
  `findutils` 0.7, the rest ~1.5; `busybox` +0.45). The real gain is
  probably translation time (`coreutils` alone ~100 ELFs): **measure the
  bootstrap per stage first.**
- **Migration:** existing 0.2.x prefixes keep the GNU base or get
  converted; say which in the release notes.
- **Maybe together:** `libc6` as Debian's exact identity also changes the
  base; doing both at once means one reinstall for users.

**Options to weigh in the design doc** (busybox is one of them; they
combine):

*Replace the GNU tools with something smaller:*
- **busybox** (above): covers almost all 10; the Essential/stand-in issues.
- **toybox** (Android's own multi-call set, also packaged in Debian):
  smaller, less complete (awk, some options), less proven for Debian's
  maintainer scripts.
- **uutils coreutils** (Rust, one binary; Debian `rust-coreutils`):
  `coreutils` only, aims at GNU compatibility (Ubuntu is adopting it);
  `sed`/`grep`/`find` stay GNU.
- **Termux's glibc tools:** a `coreutils` stand-in pointing at Termux's
  `coreutils-glibc` (installed already, on maintainer scripts' `PATH`),
  like the `libc6` stand-in: nothing to download or translate, GNU
  behaviour kept.

*Keep the GNU Debian base, install it faster:*
- **Prebuilt base:** build and translate the base once (CI /
  `deb-native-repo`), ship it as a tarball; the bootstrap downloads and
  unpacks it -- skips most of translate (41s) + configure (23s), and the
  base stays fully Debian. Translated files embed the prefix path, so it
  fits the default `~/.dn` (the same on every Termux); other paths fall
  back to today's local bootstrap. Likely the largest win; close to how
  Termux itself installs (a bootstrap zip).
- [x] **Parallel translation** (released in v0.2.2-prealpha, `DN_JOBS`, default
  the CPU count -- `nproc` says 4 on fe2): translate 41s -> **14s**, fresh
  install 1m37s -> **1m5s** (fe2, 20:42). Configure (21s) is now the
  largest stage.
- **Slimmer configure:** find what the 23s is (probably
  `ca-certificates`' rebuild, `debconf`) before cutting.
- **Clean the apt cache after bootstrap** (-85 MB): trivial, could go
  into 0.2.x.

Leaning: prebuilt base + cache cleaning (+ parallel translation) first;
a busybox base only if size still matters after that.

**Baseline (0.2.0, fe2, 2026-09-27 20:02, fresh install, tracer prebuilt):**

| stage | time |
|---|---|
| runtime (cached build) | 0s |
| Debian index | 6s |
| signatures | 5s |
| stand-ins | 5s |
| download base | 4s |
| **translate** | **41s** |
| unpack | 6s |
| **configure** | **23s** |
| launchers, PATH | 7s |
| **total** | **1m37s** (the first install that day: 3m13s, tracer built and a slower network) |

Prefix: **224 MB, 2,233 files**; of that **85 MB `var/cache/apt`** (the
bootstrap's downloaded/translated `.deb`s) and **56 MB `var/lib/apt`**
(lists) -- the installed base itself is ~83 MB. Cleaning the cache after
bootstrap is a cheap win, independent of busybox.

**0.4.0 is done when**, against this baseline and the same seeded survey
sample (`docs/survey-0.2.0/`):

| measure | 0.2.0 | 0.4.0 goal |
|---|---|---|
| fresh install (fe2, same conditions) | 1m37s | clearly less; translate + configure (64s) are the target |
| base packages | 23 | fewer |
| prefix size | 224 MB | smaller (cache cleaned + lighter base) |
| survey: installed | 99 / 100 | >= 99 |
| survey: programs working within limits | 98 / 100 | >= 98 |
| per-package regressions | -- | none |
| maintainer scripts | pass | still pass: `ca-certificates`, `passwd`, `fastfetch`, a shell package (`add-shell`), `update-alternatives` users |

## 0.5.0 roadmap: our own glibc (Debian's source + Android patches)

**Reprioritized 2026-09-30**, ahead of 0.4.0: the tracer/loader discussion
above (fake-root section, "Speed under the tracer") landed on this as the
real fix for tracer-only case #2 (NSS), not a stop-gap -- Android's kernel
and SELinux only enforce at the syscall boundary, so a glibc we build and
patch ourselves is free to make NSS read the prefix's `/etc` directly, the
same way Termux already ships a working patched glibc on Android. Still
the highest-cost item on this file (own build pipeline, patch series
maintained per glibc version), so sequence it against 0.4.0/alpha by
actual bandwidth, not just this note.

**Scope check, closed 2026-09-30:** own-glibc is not a general fix for
everything Android breaks -- see
[`docs/android-seccomp-audit.md`](docs/android-seccomp-audit.md). Three
gates exist below glibc entirely, and own-glibc has no leverage on any of
them (a syscall failing there fails the same way no matter which library
issued it): **A) the app seccomp allowlist** (`bionic/libc/SECCOMP_
ALLOWLIST_APP.TXT`/`_COMMON.TXT`, confirmed against upstream AOSP source
-- `io_uring*` is absent from it entirely); **B) capability/kernel-config**
(`CAP_SYS_ADMIN`, `CONFIG_USER_NS` compiled out); **C) SELinux**, found
while testing this discussion -- a plain `AF_NETLINK` `bind()` passes
seccomp and needs no capability, yet still fails `EPERM` under
`untrusted_app_27`'s policy. Own-glibc's actual, confirmed leverage stays
the NSS/loader-internal-path class only (NSS, `gconv`, locale,
`ld.so.cache`, `RUNPATH` -- one mechanism, several symptoms) plus
whatever syscall stock Debian `libc6` trips at startup (still unnamed,
low-cost to check later).

The audit's relevance triage also found most of what looked like scope
isn't: namespaces/mount/overlayfs, `swapon`, `mknod`, SysV IPC, and ports
<1024 don't matter to this project's actual goal (a dev-tool/CLI
userland, not containers or a network appliance) and were dropped without
needing a single on-device test. The one real candidate (`ping`/`ip`) was
tested directly: `ping` already works (Android's sandbox allows
unprivileged ICMP); `ip`-class netlink is Gate C and out of scope, same
as the rest. `io_uring` (Gate A, confirmed real) is deliberately deferred
-- an HPC/high-throughput concern, not this project's.

Theme: match mainstream Debian for real. The prefix's `libc6` stops being
an imposter (Termux's glibc under Debian's name, 0.4.0) and becomes
**Debian's own glibc source, at Debian's exact version, with the Android
patches applied by our own patch pipeline**.

What it buys:
- `libc6`, `libc6-dev`, `libc-bin`, `locales` at Debian's exact version,
  headers matching the runtime -- no faked identity;
- follows Debian: point releases and security fixes by rebuilding;
- workarounds go, since we choose the build configuration: NSS reads the
  prefix's `/etc` (no priv `getent`, no tracer for lookups); the loader's
  default search path is the prefix's (no per-ELF RUNPATH patching);
  `ld.so.conf.d` and `ldconfig` work;
- Debian's own `perl`/`python3` safe to rely on (no Termux 5.42 vs
  Debian 5.40 gap).

What it needs:
- [x] **patch series:** termux-pacman's `glibc-packages` Android patches,
      forked as our own series (decided: fork `termux-pacman`'s patches
      retargeted, not an independent rewrite -- proven working, far less
      redundant effort). **Full per-file reading + fork pass done
      2026-09-30**, see `docs/android-seccomp-audit.md`'s "Full per-file
      fork verdict" (all 54 files in `gpkg/glibc/` judged) and "Everything
      marked 'fork' actually forked" (the actual work, onto
      `~/dn-glibc-build/work`, regenerated + round-trip-verified into
      [`third_party/glibc-android-patches/dn-glibc-android.patch`](third_party/glibc-android-patches/dn-glibc-android.patch),
      147 file-diffs up from 67). Owner's rule applied throughout: don't
      fake a feature that genuinely doesn't exist, report it honestly,
      except an important feature, which needs deliberate consideration.
      `fakesyscall.json` turned out to have three buckets under that rule,
      not the two assumed earlier -- a real substitute for the missing
      syscall (not a fake at all -- this also corrects the earlier
      "`shmem-android.c`: defer" call, since its `shmat`/`shmctl`/`shmdt`/
      `shmget` route here, not through a fake), an honest `ENOSYS`, and the
      actual fake (`setuid`/`setgid`/... unconditionally succeed). Only
      the fake bucket is parked (below); everything else -- including
      `android_passwd_group.c` and `shmem-android.c`, both read in full and
      confirmed low-risk/real -- is forked now. One patch
      (`disable-termios2.patch`) targets glibc internals that no longer
      exist in this shape in 2.41 and needs a real port, not a mechanical
      reapply -- not forked yet, terminal I/O may still misbehave on
      Android without it. **Not yet built or tested on-device** -- next
      step, either via CI (`build-glibc.yml`) or on-device now that `cc1`'s
      `ET_EXEC` segfault (`findings.md`, 2026-09-30) no longer blocks a
      native build attempt;
- [ ] **build pipeline:** cross-build in CI (too slow on a phone),
      producing `libc6`, `libc6-dev`, `libc-bin`, `locales` `.deb`s
      versioned like Debian's (e.g. `2.41-12+deb13u4+dn1`);
- [ ] **publishing:** `deb-native-repo`, shared with the prebuilt base
      (0.4.0 option) -- one pipeline for both;
- [ ] **fixed prefix path** built in (`/data/data/com.termux/files/home/.dn`,
      the same on every Termux) -- settles the open "prefix location"
      question;
- [ ] **`dn-trace` upgrade** (bundled into 0.5.0 on purpose, decided
      2026-09-30 -- not a separate release). After own-glibc removes
      tracer-only case #2 (NSS), the tracer's permanent job stays exactly
      static binaries and raw `syscall()` (#3/#4 -- no libc call to
      interpose on regardless of whose glibc it is, own-glibc has no
      leverage here). Two independent upgrades on top of that unchanged
      core job, **prioritized 2026-09-30**:
      1. **First: clean death instead of a kill** (see
         `docs/android-seccomp-audit.md`, "Phase 5 idea" -- Gate A only).
         A syscall absent from Android's seccomp allowlist doesn't return
         an errno, it `SIGSYS`-kills the whole process; `tracer/tracee/
         seccomp.c` already catches this for `set_robust_list` (currently
         answers with a fake success, harmless because that call is
         best-effort). Extend the same catch to answer with **`ENOSYS`**
         for other Gate-A syscalls (starting with `io_uring_setup`/
         `_enter`/`_register`, absent from both allowlist TXT files) --
         honest "not available here," not a fake success, matching what a
         kernel without that syscall already returns. No library patch,
         no own-glibc dependency -- purely this file. Needs `dn-run.c`'s
         `classify()` extended too (an `io_uring`-linked *dynamic* glibc
         binary runs shim-only today, no `ptrace` attached at all, so
         nothing to catch until it's routed through the tracer the same
         way NSS-importers already are).
      2. **Speed (`SECCOMP_RET_USER_NOTIF` instead of `ptrace`), reconsidered
         2026-09-30 -- not committed for this version.** Still flagged as
         "the endgame" in `docs/direct-usage.md` (:16, :102),
         `docs/syscall-boundary.md` (:131), `docs/shim-coverage.md` (:175),
         but explicitly deferred for now: it cannot rewrite a syscall's
         arguments in place the way `ptrace` can (path rewriting, the
         tracer's main job, would need `process_vm_writev` on the tracee's
         existing argument buffer before `SECCOMP_USER_NOTIF_FLAG_CONTINUE`),
         so it adds real implementation weight to `dn-trace` for a
         performance gain, not a correctness one -- risks growing the
         tracer just as the goal is to keep its scope down to the two
         permanent cases. Revisit only after #1 ships and only if `dn-trace`
         is still small; not a 0.5.0 blocker.
- [ ] **proof:** survey before/after, `gcc` hello-world, NSS without the
      tracer.

**Status (2026-09-30, `fe2`): patch fork started, bootstrap-via-real-gcc
blocked by a new bug, unrelated to own-glibc.** Motivation sharpened first:
`dn-adopt.sh` today symlinks the prefix's `libc6` to *whatever glibc Termux
has installed live* -- deb-native does not pin its own version, it drifts
with Termux's updates. Vendoring is a version-pinning fix, not only the NSS
fix above.

Fetched Debian's real `glibc` source package (`2.41-12+deb13u4`, same
version already used for the stock-segfault test), applied Debian's own
~80-patch quilt series with `quilt push -a` (all applied cleanly), then
forked and applied `set-dirs.patch` + `disable-clone3.patch` from
`termux-pacman/glibc-packages` on top, retargeted to this project's fixed
prefix (`/data/data/com.termux/files/home/.dn`) instead of
`@TERMUX_PREFIX@`/`@TERMUX_PREFIX_CLASSICAL@`. 4 of `set-dirs.patch`'s ~66
touched files needed hand-fixing (context drift: Debian's own patches
already changed `_PATH_VARDB`, `nscd`'s db path, etc. from what
`termux-pacman`'s patch assumed) -- done, verified by inspection, not yet
by a build.

**Build-step blocker resolved 2026-09-30 (was: "blocked, not by own-glibc's
design").** Vanilla `clang` still cannot build glibc from source
(`configure`'s "redirection of built-in functions" check needs GCC-specific
`__asm`-labeled `extern` behavior clang has never implemented), so a real
GCC is still required -- Debian's own `gcc-14`/`binutils` (arm64), installed
through deb-native's own `apt-get`, is the plan. That install hit a second,
unrelated bug: **`cc1` (an `ET_EXEC`, non-PIE binary) segfaulted
immediately after glibc's loader started.** Root-caused (not `ld-dn`'s
runtime logic, the original suspicion): `dn-translate-deb.sh`'s
`patchelf --set-rpath` corrupted `cc1`'s program header table -- inserting
a RUNPATH from scratch on a tightly-packed `ET_EXEC` binary with no layout
slack produced two overlapping `PT_LOAD` segments (one `RW`, one `R E`),
and the kernel's own `execve()`-time mapping of the second clobbered
`cc1`'s `PT_DYNAMIC`, which glibc's loader then read garbage from. Fixed
by moving RUNPATH out of the static per-`.deb` patch step entirely: `ld-dn`
now sets `LD_LIBRARY_PATH` (covers the whole load graph transitively, which
per-file RUNPATH patching never did) and `COMPILER_PATH` (a second,
separately-found gap -- gcc's own subprogram search doesn't fall back to a
plain `$PATH` walk, so without it gcc silently ran Termux's own `ld.lld`
instead of the prefix's binutils) in the environment it already builds per
launch. `dn-translate-deb.sh`/`dn-adopt.sh`'s ELF patching drops to
`--set-interpreter` only -- the one thing confirmed unable to move to the
loader (kernel reads `PT_INTERP` at `execve()`; no `binfmt_misc` escape
hatch on this device, checked). Full diagnostic writeup:
[`docs/findings.md`](docs/findings.md), "patchelf corrupting an `ET_EXEC`
binary's program headers". Verified: `cc1 -v` runs, `gcc-14 -S` produces
correct assembly, `gcc-14 -nostdlib -static` links against the prefix's own
`ld` with no manual env needed.

**Next**, now unblocked: actually attempt building the forked glibc
on-device with this now-working `gcc-14`, as an alternative to the slower
CI round-trip -- separately, the CI-built artifact (commit `538555d`) still
hits `SIGSYS` on its own startup (`android-seccomp-audit.md`, "0.5.0 first
build attempt"), needing `fakesyscall.json` forked in too; that gap is
unrelated to this bug and still open.

`ld-dn` stays: Android's root has no `/lib/ld-linux-aarch64.so.1`, so
programs still need their interpreter pointed into the prefix.

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

**Next (0.4.0, temporary until 0.5.0's own glibc): `libc6` as Debian's
exact identity.** The stand-in wraps
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
      **Design: systemd's face, runit's body.** Real systemd is out (PID 1,
      or `--user` with cgroups and a session bus: Android gives an app
      none); mimicking all of systemd is a trap. So: packages keep shipping
      units and calling `deb-systemd-helper`/`systemctl`; deb-native
      translates each unit at install (`ExecStart` foreground as the app
      user, `Environment`/`EnvironmentFile`, `WorkingDirectory`,
      `RuntimeDirectory`/`StateDirectory` -> `$DN/run/NAME`,
      `$DN/var/lib/NAME`; `User=` and sandbox options dropped; low ports,
      capabilities, devices -> refused with the reason); a small `systemctl`
      front maps `start/stop/restart/status/enable/disable` to `sv` and the
      service links, anything else says "not supported" (prior art:
      `docker-systemctl-replacement`). Rule: translate what maps to a
      supervised process, refuse the rest -- no growing systemd features.
      Needs first: `/run` in the shim, system users in the prefix's
      database (overlaps with sudo modes). Test ladder: `cron` -> `redis`
      (a system user, a data dir) -> `dbus` (a socket in `/run`). Android
      may kill background services (phantom-process killer, battery
      optimisation): document the wake lock / battery settings.
- [ ] 2. **sudo modes** (below): fake root + `base-passwd`'s users so
      `adduser`/`chown service-user` in maintainer scripts succeed.
- [ ] 3. Both together: packages that need a system user *and* run a
      daemon -- sudo-less's service support, matched.

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
      cannot rewrite syscall arguments). Plan
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
      - [x] Termux `proot` fallback dropped from `dn-run.c` (0.2.3): the
            loader, the shim and `dn-trace` cover the prefix; without
            `dn-trace`, `dn-run` warns and runs untranslated.

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
