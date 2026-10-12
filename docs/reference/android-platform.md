# Android platform limits: the three enforcement gates, and the glibc patch

<!-- template: templates/docs.template.md -->

What stops a syscall on this device, independent of which library issued
it, and the current fork verdict for Termux's Android glibc patch series
against that taxonomy. Extracted from
`../log/android-seccomp-audit.md` (the
investigation that found this) to keep as a standing reference instead of
buried in that log — update this doc, not the log, when the taxonomy or
the patch's own fork status changes; the log stays the historical record
of how it was found.

## Contents

- [The three enforcement gates](#the-three-enforcement-gates)
- [Device probe: sandbox limits confirmed directly](#device-probe-sandbox-limits-confirmed-directly)
- [Known findings, by gate](#known-findings-by-gate)
- [Termux's Android glibc patch: catalog and fork verdict](#termuxs-android-glibc-patch-catalog-and-fork-verdict)

## Related docs

- `../log/android-seccomp-audit.md` —
  the investigation that produced this doc, plus the on-device glibc
  build attempt log.
- This doc's own "Device probe: sandbox limits confirmed directly"
  section was originally a `docs/log/findings.md` entry ("Platform
  sandbox limits, by direct probe"), moved here in full rather than
  split out separately when the log was later split into
  `docs/log/findings/`.
- [`design.md`](../spec/runtime.md) — "Fake root", whose
  `set-fakesyscalls-parked.patch` exclusion is permanent (decided, not
  pending), per this doc's per-file fork verdict and
  [`runtime.md`](../spec/runtime.md) principle 3.
- `../../patches/README.md` —
  the actual patch this doc's catalog describes.

## The three enforcement gates

A syscall can fail at any of three independent points, before the calling
library's own code is ever considered:

**Gate A — seccomp-bpf filter**, installed by zygote into every app
process (`Seccomp: 2`, confirmed by direct probe — "Device probe: sandbox
limits confirmed directly", below). Default-deny: a syscall not
on the allowlist is refused (historically SIGSYS/crash — "Android O
crashes an app that uses an illegal syscall", see sources). The allowlist
is assembled from:

- [`bionic/libc/SYSCALLS.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SYSCALLS.TXT)
  — every syscall bionic itself exposes a wrapper for.
- [`bionic/libc/SECCOMP_ALLOWLIST_APP.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SECCOMP_ALLOWLIST_APP.TXT)
  — app-process additions. Confirmed present (fetched 2026-09-30):
  `pipe`, `access`, `stat64`, `open`, `getdents`, `eventfd`, `epoll_wait`,
  `epoll_create`, `creat`, `unlink`, `lstat64`, `fcntl`, `fork`, `poll`,
  `inotify_init`, `getuid`, `remap_file_pages`, `rename`, `mmap`, `dup2`,
  `compat_select:_newselect`, `mkdir`, `renameat`.
- [`bionic/libc/SECCOMP_ALLOWLIST_COMMON.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SECCOMP_ALLOWLIST_COMMON.TXT)
  — shared with other domains. Confirmed present: `pivot_root`,
  `ioprio_get`/`_set`, `gettid`, `futex`(`_time64`), `clone`/`clone3`,
  `sigreturn`/`rt_sigreturn`, `rt_tgsigqueueinfo`, `restart_syscall`,
  `riscv_hwprobe`, `vfork`, `perf_event_open`, `tkill`, `seccomp`, `open`,
  `stat64`/`stat`, `readlink`, the `io_*` AIO family (`io_setup`,
  `io_submit`, ... — **not `io_uring_setup`/`_enter`/`_register`**,
  confirming [`runtime-failures.md`](../spec/runtime.md)'s "`io_uring` not
  intercepted" is really "not even on the app allowlist"), `execveat`,
  `membarrier`, `userfaultfd`, the `_time64` clock/timer family,
  `pselect6_time64`, `ppoll_time64`, `recvmmsg_time64`,
  `rt_sigtimedwait_time64`, `futex_time64`, `sched_rr_get_interval_time64`.
- Assembly logic lives in
  [`libc/seccomp/seccomp_policy.cpp`](https://android.googlesource.com/platform/bionic/+/704772bda034448165d071f68b6aeca716f4220e/libc/seccomp/seccomp_policy.cpp)
  (per-arch, generated at build time from the TXT files above).
- Background: [Android Developers Blog, "Seccomp filter in Android
  O"](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html)
  — "blocks 17 of 271 syscalls in arm64" is the **2017/O baseline**, not
  this device's policy (`main` branch's TXT files above are current AOSP,
  years of additions since O; the device here is Android 16 — treat the
  blog's number as historical context only, not a live count).
- [source.android.com: Application Sandbox](https://source.android.com/docs/security/app-sandbox)
  — the general sandboxing model (uid-per-app, SELinux, seccomp stacked
  together, matching the device probe above).

**Gate B — capability / kernel-config**, independent of seccomp: a syscall
can be *allowed* by the filter and still fail, because it needs a
capability the app uid never has (`CapEff = 0`, confirmed by probe) or a
kernel feature compiled out. Confirmed example: `pivot_root` **is** on
`SECCOMP_ALLOWLIST_COMMON.TXT` — it passes Gate A — but needs
`CAP_SYS_ADMIN`, which is `0` in both the Termux app domain and the
seccomp-free shell domain. Same shape as `CLONE_NEWUSER` -> `EINVAL`
(`CONFIG_USER_NS` off, kernel-wide, no app or seccomp involvement at all)
and `CLONE_NEWNS`/`mount` -> `EPERM` (`CAP_SYS_ADMIN` missing).

**Gate C — SELinux policy**, independent of both: a syscall can pass A
(on the allowlist) and need nothing from B (no missing capability or
kernel config) and still fail, because the app-domain SELinux policy
denies the specific operation. Found via `ip`-class netlink (passes A,
needs nothing from B, still blocked) — distinct from the kernel/capability
layer Gate B covers.

**Why the gates matter for own-glibc's scope:** own-glibc changes what
glibc does at the *libc* layer (sysconfdir, default search paths, NSS
dispatch) — it cannot move a syscall through Gate A, B, or C, because all
three are enforced by the kernel/zygote/policy before the syscall's
arguments (or the calling library) are ever inspected. A syscall failing
any gate fails the same way no matter whose glibc issued it. Only the
*shim's* territory (libc-internal path resolution) is where own-glibc has
any leverage at all.

| gate | enforced by | own-glibc fixes? | tracer fixes? |
|---|---|---|---|
| A: seccomp allowlist | zygote, per-syscall | no | yes, by SIGSYS emulation (already done for `set_robust_list`) |
| B: capability / kernel config | kernel, per-capability or compile-time | no | no |
| C: SELinux | policy, per-syscall/resource | no | no (same as B — a policy/kernel decision, not a missing translation) |
| neither (libc-internal path choice) | glibc's own build config | **yes** | n/a (this is what `dn-trace`'s NSS route works around today, but own-glibc removes the need for that route entirely) |

## Device probe: sandbox limits confirmed directly

The evidence behind the gate taxonomy above, from probing the device
directly (2026-09-26) rather than reasoning from AOSP source alone:
Termux's own app uid (`untrusted_app_27`) and the seccomp-free Android
shell uid (`u:r:shell:s0`, reached with `dsh`, uid 2000). Android 16
userspace, kernel 5.10.240, arm64.

| | Termux app uid | shell uid (`dsh`) |
|---|---|---|
| uid / SELinux | 10663, `untrusted_app_27` | 2000, `u:r:shell:s0` |
| CapEff | 0 | 0 |
| seccomp | filter active (`Seccomp: 2`, 1 filter) | **none** (`Seccomp: 0`) |
| `unshare(CLONE_NEWUSER)` | `EINVAL` | `EINVAL` |
| `unshare(CLONE_NEWNS)` | `EPERM` | `EPERM` |
| `/dev/fuse` | — | `crw------- root root` (unreadable) |
| read `$PREFIX` | yes (owner) | no (`/data/data/com.termux` permission denied) |
| `run-as com.termux` | — | `package not debuggable` |

Findings:

- **User namespaces are off kernel-wide, not an app restriction.**
  `unshare -U` returns `EINVAL` even from the seccomp-free shell, and
  `/proc/self/ns/` has no `user` entry — the kernel is built without
  `CONFIG_USER_NS`. This is the hard reason sudo-less's kernel "view" (user +
  mount ns + unprivileged overlayfs) cannot exist here: no sandbox or
  permission unlocks it.
- **Mount namespaces and overlayfs are unreachable.** `unshare(CLONE_NEWNS)`
  and `mount` need `CAP_SYS_ADMIN`; `CapEff` is `0` in both domains and
  SELinux is enforcing. The kernel does list `runtime` and `fuse` in
  `/proc/filesystems`, but runtime needs mount privileges and `/dev/fuse` is
  root-only — neither is usable.
- **`ptrace` works in the app domain.** `proot -0` runs in Termux (fake uid 0,
  context still `untrusted_app_27`), so a `ptrace`-based syscall tracer is
  feasible from the app uid. A tracer in the shell domain is not: cross-domain
  tracing would need `CAP_SYS_PTRACE` and access to the app prefix, both absent.
- **`dsh`/shell is a probe and provisioning tool only.** Its lack of a seccomp
  filter makes it useful for reading the kernel's real state, but it cannot
  host the enforcement layer — it can neither read the private prefix nor
  `run-as` the app.
- **The app already runs under one seccomp filter.** Any filter a project adds
  stacks on top (most restrictive wins); Android's baseline cannot be lifted.
  That baseline, plus SELinux, is why `CLONE_NEWUSER` is denied even where
  seccomp is off.

Net: the only mechanisms available are (1) libc-level interposition — the
shim this project ships — and (2) a syscall-level tracer via `ptrace` /
`SECCOMP_RET_USER_NOTIF` **inside the app uid**. Namespaces, overlayfs and
FUSE are off the table by kernel and SELinux policy, exactly as the design
assumed.

## Known findings, by gate

| finding | where recorded | gate | own-glibc fixes? |
|---|---|---|---|
| Debian's stock `libc6` **starts and runs under the tracer** (re-tested 2026-10-12, `beyond0q`); the earlier "killed at startup" was the seccomp `TRAP` on `rseq`/`clone3` with **no tracer attached** | this doc, ["Stock libc6 re-tested"](#stock-libc6-re-tested-the-startup-crash-was-an-unhandled-seccomp-sigsys) | A — `rseq` (#293) / `clone3` (#435) `SECCOMP_RET_TRAP`, answered by the tracer's existing SIGSYS→ENOSYS emulation | **no** — the tracer already answers it (`src/tracer/tracee/seccomp.c`); glibc needs no startup patch |
| `set_robust_list` SIGSYS on static binaries | [`tracer.md`](../spec/runtime.md) | A | no (tracer's SIGSYS emulation already answers it) |
| NSS opens (`getpwnam`, ...) via `__open_nocancel` | [`shim-coverage.md`](../spec/runtime.md), [`syscall-boundary.md`](syscall-boundary.md) | neither — not a kernel block, a *libc-internal symbol binding* choice | **yes** — this is the case own-glibc already targets |
| `gconv`/locale modules, `ld.so.cache`, `RUNPATH` | [`runtime-failures.md`](../spec/runtime.md) B | neither — same as NSS, loader-internal path choice | **yes**, same mechanism as NSS |
| SysV IPC (`shmget`/`semget`/`msgget`) | [`runtime-failures.md`](../spec/runtime.md) E | A or B | out of scope — not a current goal (`../log/android-seccomp-audit.md`'s relevance triage) |
| `mount`, `pivot_root`, `swapon`, netlink, TUN, ports <1024, `mknod` | [`runtime-failures.md`](../spec/runtime.md) E | B (`CAP_SYS_ADMIN`/similar missing; `pivot_root` confirmed Gate-A-allowed) | no |
| `CLONE_NEWUSER` | device probe, above | B, kernel-wide (`CONFIG_USER_NS` off) | no — not even a seccomp question |
| `CLONE_NEWNS` | device probe, above | B (`CAP_SYS_ADMIN`) | no |
| `io_uring*` | [`runtime-failures.md`](../spec/runtime.md) A, H | A — absent from both allowlist TXT files | no (tracer would need to emulate it; not attempted) |
| `ip`-class netlink | `../log/android-seccomp-audit.md` | C (SELinux) | no |

Everything but the NSS/loader-internal row is Gate A, B, or C,
library-agnostic — own-glibc's scope should be understood as "fixes the
NSS/loader-internal row and whatever startup syscall stock `libc6` trips,"
not a general fix for "things Android breaks."

### Stock `libc6` re-tested: the startup crash was an unhandled seccomp SIGSYS

The row above used to read "Debian's stock `libc6` killed at startup …
glibc's own dynamic-linker startup, before any syscall." That
classification does not survive a re-test on the target device. What was
observed was an **unhandled seccomp `SIGSYS` at the syscall boundary**, not
a fault under it — so it sits in the lane the tracer already owns.

**Setup.** SM-G970W (`beyond0q`), Android 12 / SDK 31, kernel
`4.14.190-23725627-abG970WVLU9IXE1`, app domain `u:r:untrusted_app_27:s0`,
`Seccomp: 2`, `NoNewPrivs: 1`. Test binaries: Debian's own
`ld-linux-aarch64.so.1` and `libc.so.6` extracted from
`libc6_2.41-12+deb13u4_arm64.deb`, byte-identical to the `.deb` (no patch,
no `PT_INTERP` rewrite). Programs: a trivial `hello`, a `pthread` + NSS +
`/etc/os-release` probe, and real prefix binaries (`bash`, `env`, `ls`,
`cat`, `sed`, `grep`, `tar`, `gzip`).

**Result.** All of them run, exit `0`, under `dn-trace` in the app domain.
The only interventions are two warnings, both from the existing emulation
in `src/tracer/tracee/seccomp.c`:

```
dn-trace warning: blocked syscall rseq (#293) denied by seccomp; returning ENOSYS
dn-trace warning: blocked syscall clone3 (#435) denied by seccomp; returning ENOSYS
```

`rseq` appears on every program (glibc registers it at startup); `clone3`
only when a thread is created — and `ENOSYS` is exactly the answer glibc
wants (rseq: "not supported, disable it"; clone3: "fall back to `clone`").
`LD_DEBUG=libs` confirms the stock object really is the one in play
(`calling init: /tmp/stock/libc.so.6`; its md5 differs from the prefix's
patched build).

**Mechanism.** Android's app seccomp policy answers `rseq`/`clone3` with
`SECCOMP_RET_TRAP`, which delivers `SIGSYS`; with no handler installed the
process dies (`Bad system call`). Reproduced directly: in `runas_app`
(`Seccomp: 0`, no Android filter) with a hand-installed filter that
`TRAP`s only `rseq`/`clone3`, stock glibc run **untraced** prints
`Bad system call` and dies, while the same program under `dn-trace` runs.
That is the whole difference — a `SIGSYS` the tracer converts to `ENOSYS`,
via `set_result_after_seccomp`.

**The outermost process.** `dn-trace` itself is the one program nothing traces,
so *its own* startup must survive unaided. It is built **static**, and a static
stock-glibc binary's only Android-denied startup syscall here is `rseq`;
`GLIBC_TUNABLES=glibc.pthread.rseq=0` (a supported glibc tunable) removes even
that — measured: with the tunable the `rseq` warning disappears, without it the
static binary hits it. `set_robust_list` was **allowed** on this device (the
static binary's startup showed no warning for it), unlike the older note about
the audit device. So no patched `libc.a` is needed to start the runtime either;
a launch environment with that one tunable is enough.

**What this does *not* say.** The measurement used `main`'s heavily forked
`dn-trace`, which already carries the seccomp-denial emulation, the exec gate and
the libc-internal path routing. The branch's `dn-monitor`/`dn-trace` is a trimmed
ptrace rebuild and does **not** have those pieces yet: the finding is the
*behaviour the branch must reproduce*, not something it inherits. Dropping the
glibc patch moves the obligation from "patch glibc" to "carry the emulation in
the runtime" — `main`'s `src/tracer/tracee/seccomp.c` (`set_result_after_seccomp`)
is the reference.

**Consequence for the glibc patch.** `dn-glibc-android.patch` is not
load-bearing for loader startup. Its startup-adjacent hunks are already
covered by the tracer (`set_robust_list`, `clone3`, `rseq` → the SIGSYS
emulation above) and its path/NSS/`@DN_PREFIX@` hunks are covered by the
tracer/shim path mapping. One hunk is actively *wrong* for the shim lane:
the `elf/rtld.c` change that nulls `state.preloadlist` (it exists because
`main`'s fused loader delivers its policy through `ld.so.preload`) would
suppress the `LD_PRELOAD` that carries `dn-shim.so` on this branch, and
must be dropped. See `../spec/dn-interp-shim.md` for the trim.

## Termux's Android glibc patch: catalog and fork verdict

0.5.0's "own glibc" milestone forks
[`termux-pacman/glibc-packages`](https://github.com/termux-pacman/glibc-packages)
(`gpkg/glibc/`) onto Debian's real glibc source
(`patches/`
has the combined patch, how to apply/regenerate it, and the build recipe).
This section is the catalog of what that upstream patch set contains and
which pieces this project's fork carries — current status, not a log of
how each verdict was reached (that's
`../log/android-seccomp-audit.md`).

Layout: `build.sh` (glibc 2.44, configure flags, install steps) plus ~50
loose files — some are unified `.patch` files against upstream glibc
source, some are whole replacement/added `.c`/`.h` files copied in before
configure.

**What `build.sh` does, in order:**
1. Deletes `clone3.S` for every arch (`clone3` disabled outright —
   independent of Gate A; a toolchain/compat decision, not a permission
   one).
2. Copies in Termux-authored replacements: `shm{at,ctl,dt,get}.c`,
   `mprotect.c`, `syscall.c`, `setfs{u,g}id.c`, `fake_epoll_pwait2.c` —
   these implement `fakesyscall.json`'s mechanism (a declarative
   syscall-number -> C-function map compiled into `disabled-syscall.h`,
   used when a syscall Android's kernel doesn't support gets called — the
   same "answer instead of crash" idea as the tracer's own SIGSYS
   emulation, except done at the libc source level instead of a tracer
   intercepting after the fact).
3. Copies in `android_passwd_group.c`/`.h` + generates `android_ids.h`
   (`gen-android-ids.sh`) into `nss/` — a custom NSS-adjacent source
   addition that synthesizes passwd/group entries from Android's own
   uid/gid space (`android_system_user_ids.h`), likely how
   `root`/`nobody`/app uids resolve without a real `/etc/passwd` entry.
4. Copies in `shmem-android.c`/`.h` into `sysvipc/` — SysV shared memory
   reimplemented in userspace for Android (real `shmget` is Gate-B/
   blocked). Not needed by this project (SysV IPC is out of scope), but
   useful precedent if that verdict is ever revisited: Termux already
   solved it once.
5. Runs `set-dirs.patch` (below).
6. Bumps `version.h` to Termux's own version string.

**`set-dirs.patch`: the real NSS/path fix, a plain, forkable source
patch.** A standard unified diff touching ~30 files (`resolv/resolv.h`,
`resolv/netdb.h`, `nss/nss_files/files-init.c`,
`nss/nss_files/files-XXX.c`, `nss/nss_compat/compat-{pwd,grp,spwd}.c`,
`nscd/nscd.h`, `misc/fstab.h`, `misc/ttyent.h`, `libio/stdio.h`, and more)
— every hardcoded `"/etc/..."`, `"/tmp/..."`, `"/var/..."` string replaced
with a `@DN_PREFIX@` or `@TERMUX_PREFIX_CLASSICAL@` placeholder,
substituted to Termux's real prefix path at build time. This is the
"neither" row's fix: NSS opens use `__nss_files_fopen("/etc/passwd")`
internally, unreachable by any external interposer — `nss/nss_files/
files-init.c`'s `register_file(cb, pwddb, "@DN_PREFIX@/etc/passwd",
0)` is the literal line already solving it for Termux's own prefix.

**Decision framework for forking a piece: "a package gets exactly one
view."** A Debian package assumes one consistent system identity —
`getuid()`, `/etc/passwd`, NSS, every API agreeing on "who am I, what does
this system look like." This project's architecture (shim, fake-root, the
tracer's `/etc` bind) exists to construct and hold that one illusion, over
a reality that has no real isolation at all (same kernel, same Android
uid, same seccomp filter as any other Termux process). Termux's patches
were written for a *different* illusion (Termux wants to honestly reflect
Android, not impersonate Debian root), so each piece is judged on whether
adopting it reinforces this project's single view or punctures it.

**`fakesyscall.json` splits into three buckets:**

1. **Real substitute, not a fake at all** (19 entries) — the target
   syscall is missing, so call a *different, real* function with the same
   actual effect: `statx` -> `statx_generic`; `accept4`/`recv`/`send` ->
   `accept`/`recvfrom`/`sendto`; `shmat`/`shmctl`/`shmdt`/`shmget` ->
   `shmem-android.c`'s real `/dev/ashmem`-backed implementation;
   `epoll_pwait2` -> `fake_epoll_pwait2.c` (a real polyfill on older
   `epoll_wait`); `close_range`/`fchownat`/`ftruncate`/`clock_gettime`/
   `getpgrp`/`unlinkat`/`symlink`/`link`/`faccessat`/`fchmodat` -> older
   syscall variants that do the same thing. **Fork candidate, no
   reservations.**
2. **Honest `ENOSYS`** (9 entries + families) — `msgctl`/`msgget`/
   `msgrcv`/`msgsnd`, `semget`/`semctl`/`semop`/`semtimedop*` (not
   `shmget` and friends, which are real per bucket 1), `io_uring_setup`/
   `_enter`/`_register`, `set_robust_list`/`get_robust_list`, `clone3`,
   `mq_open`, `open_by_handle_at`, `rseq`, `pidfd_send_signal`/
   `pidfd_getfd`, `mbind`/`get_mempolicy`/`set_mempolicy`, `kcmp`,
   `landlock_create_ruleset`. **Fork candidate, no reservations.**
3. **Actual fake** (the `"0"` bucket) — `setuid`/`setgid`/`setreuid`/
   `setregid`/`setresuid`/`setresgid`/`setfsuid`/`setfsgid` (+ syscall
   `1008`) unconditionally return success. Plenty of ordinary programs
   call `setuid(getuid())`/`seteuid()` as a harmless drop-privilege idiom
   even when already unprivileged, so an honest `EPERM` would break things
   that work today — but Termux's version fakes success *unconditionally*,
   with no way to tell "harmless no-op" from "a real privilege change was
   wanted." This duplicates, at the glibc layer, what this project's own
   fake-root shim does at a different layer
   (`src/dn-shim.c`'s `chown`/`set*id`/`setgroups`/`initgroups`
   refused-for-lack-of-rights -> succeed). **Parked, not forked**, pending
   fake-root's own direction — forking this bucket as-is would reinstate
   that behavior unconditionally at the glibc layer independent of
   whatever fake-root's own fate ends up being.

**`android_passwd_group.c`** — wired in as a **fallback only**, appended
to the tail of glibc's own `getXXbyYY.c` NSS chain. It only runs **after**
the normal `files` lookup against the prefix's own `/etc/passwd`/
`/etc/group` has already failed to find a match (e.g. a file legitimately
owned by some other real Android app uid fake-root never touched) — it
does not compete with fake-root's `getuid()`/`stat`-owner answers at all.
**Fork candidate, low risk.**

**`shmem-android.c`/`.h`** — real client-server IPC, not a small shim:
creates regions via `/dev/ashmem` (`ASHMEM_SET_NAME`/`_SET_SIZE` ioctls),
then runs a background listener thread per process on an
abstract-namespace Unix socket (`sun_path[0] == '\0'`,
`ANDROID_SHMEM_SOCKNAME`) that answers other processes' requests for a
given `shmid` by passing the ashmem fd over `SCM_RIGHTS`. Untested for
this project: whether `/dev/ashmem` opens cleanly from the Termux app uid,
and whether the app-domain SELinux policy (Gate C) permits abstract-socket
`connect()`/`accept()` between two of this project's own processes.
**Deferred, same as `io_uring`** — SysV IPC is out of scope, not worth
risk-grading deeper right now.

### Per-file verdict, everything in `gpkg/glibc/`

| verdict | files |
|---|---|
| **Fork — already decided/verified working** | `set-dirs.patch` |
| **Fork — real substitute (bucket 1) or its companion source** | `fake_epoll_pwait2.c`, `shmat.c`, `shmctl.c`, `shmdt.c`, `shmget.c`, `shmem-android.c`, `shmem-android.h` |
| **Fork — honest `ENOSYS` (bucket 2) or its wiring** | most of `set-fakesyscalls.patch` (the SysV-IPC/`statx`/`mq_open`/`open_by_handle_at`/`epoll_pwait2`/`close_range` hunks), `sysvipc-Makefile.patch` |
| **Fork — `fakesyscall.json`'s dispatch mechanism itself** | `fakesyscall.json` (buckets 1+2 only, see "Not forked yet"), `fakesyscall.h`, `fakesyscall-base.h`, `syscall.c`, `syscall.S.patch`, `unistd.h.patch` (declares the renamed `syscallS`), `set-sigrestore.patch` (small `#include` glue for the same mechanism) |
| **Fork — real, independently-confirmed Android kernel/ABI fixes, not fakesyscall-related** | `disable-clone3.patch` (also independently in bucket 2's `clone3` — either route works), `dl-execstack.c.patch` (Android's real W^X enforcement on stack pages), `kernel-features.h.patch` (no separate `accept`/`recv`/`send` syscalls — needed for sockets to work at all), `clock_gettime.c.patch`, `faccessat.c.patch`, `fchmodat.c.patch`, `fstatat64.c.patch` (all: prefer an older syscall variant Android actually has), `sem_open.c.patch` (`link()` -> `symlink()` — independently confirmed by this project's own finding: `dn-translate-deb.sh`'s own comment already documents "Android refuses `link(2)` in app data (EACCES)"), `set-nptl-syscalls.patch` (drops `set_robust_list` calls from pthread create/fork/TLS-init entirely — same Gate-A problem the tracer already SIGSYS-emulates, fixed further upstream), `set-static-stubs.patch` (static-linking unwind glue, low risk either way) |
| **Fork — real Android-specific fix, found outside `fakesyscall.json`** | `mprotect.c` (Android's W^X blocks `mprotect(..., PROT_EXEC)` on an *existing* mapping — real Termux issue #49, real fix via remap; matters for any JIT) |
| **Fork — NSS identity fallback, risk-graded above** | `android_passwd_group.c`, `android_passwd_group.h`, `android_system_user_ids.h`, `gen-android-ids.sh`, `getXXbyYY.c.patch`, `getXXbyYY_r.c.patch`, `getgrgid.c.patch`, `getgrnam.c.patch`, `getpwnam.c.patch`, `getpwuid.c.patch` |
| **Fork — build glue for whatever the above pulls in** | `misc-Makefile.patch`, `misc-Versions.patch`, `nss-Makefile.patch`, `posix-Makefile.patch` |
| **Not applicable, empirically confirmed — not just hard to port** | `disable-termios2.patch`: targets a `termios2`-based `isatty`/`tcgetattr`/`tcsetattr` implementation that no longer exists anywhere in 2.41's source; 2.41 already uses plain `TCGETS`/`TCSETS` unconditionally, confirmed live via `dn-trace`. Not forked, not needed. |
| **Needs its own careful evaluation — not a simple fork/skip** | `set-ld-variables.patch`: adds a parallel `GLIBC_LD_*` env-var namespace (`GLIBC_LD_LIBRARY_PATH`, `GLIBC_LD_PRELOAD`, ...), checked *before* the plain `LD_*` name, to keep Android's own Bionic linker from reacting to the same env vars a glibc child inherits. This lands squarely on top of the project's own glibc-child environment. Read the *reason* this exists (what actually breaks without it, in this project's own process tree, not Termux's) before deciding. |
| **Not forked yet — parked on the fake-root decision (bucket 3 above)** | the `"0"`-bucket entries inside `fakesyscall.json` (`setuid`/`setgid`/`setreuid`/`setregid`/`setresuid`/`setresgid`/`setfsuid`/`setfsgid`), `setfsuid.c`, `setfsgid.c`, and the `set-fakesyscalls.patch` hunks touching `setegid.c`/`seteuid.c`/`setgid.c`/`setregid.c`/`setresgid.c`/`setresuid.c`/`setreuid.c`/`setuid.c`/`local-setxid.h` (the file otherwise forks now, per bucket 2 above — only these specific hunks wait) |
| **Defer — real feature, just not urgent for the Alpha goal** | `locale-gen`, `locale.gen.txt` (locale generation — i18n, not blocking compilers/languages), `syslog.c` (routes `syslog()` to Android's real `logd` via `/dev/socket/logdw` — a genuine integration, just not urgent) |
| **Not applicable — wrong architecture** | `i386-syscalls.list.patch`, `glibc32.subpackage.sh` (i386/32-bit; this project is arm64-only) |
| **Generic build plumbing, not an Android patch** | `sdt-config.h`, `sdt.h` (SystemTap probe-point support, vendored/pre-generated rather than Android-specific) |
| **Not a patch — the build driver itself** | `build.sh` (reference when building this project's own pipeline, not "fork or don't") |

This completes the catalog — nothing in `gpkg/glibc/` is unaccounted for.
