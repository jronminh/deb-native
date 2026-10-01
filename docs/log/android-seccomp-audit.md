# Android seccomp/capability audit (started 2026-09-30)

Why: `../spec/runtime-failures.md` and `findings.md` list several things
Android blocks (`libc6` killed at startup, `set_robust_list` SIGSYS, SysV
IPC denied, `mount`/`CLONE_NEWNS` `EPERM`, `CLONE_NEWUSER` `EINVAL`), but
each was found ad hoc, one program at a time. Before scoping 0.5.0 ("our
own glibc") on the assumption that *rebuilding glibc* fixes everything
Android breaks, we need to know which failures are actually **glibc's own
choices at build time** (sysconfdir, search paths -- own-glibc fixes these)
versus **the kernel/seccomp refusing the syscall outright, regardless of
which library issued it** (own-glibc cannot fix these; only the tracer's
SIGSYS-emulation trick, or nothing, can). Method requested: check AOSP
docs/source first, then test on-device, then conclude -- in that order,
not the reverse.

Phase 1 (the three enforcement gates) and Phase 2 (reconciling known findings against them) have been extracted as a standing reference: see [`../spec/android-platform.md`](../spec/android-platform.md) — "The three enforcement gates" and "Known findings, by gate". What follows here is the investigation from Phase 2b onward.

## Phase 2b: relevance triage against project scope (2026-09-30)

Before spending Phase 3 effort on-device: not every Gate-B/Gate-A feature
matters to this project. Scope is `TODO.md`'s Alpha goal -- compilers,
popular languages (`python3`+numpy, `perl`+XS, `ruby`, `nodejs`), `apt`
upgrade/remove, clear refusals, a `gcc` demo. This is a dev-tool/CLI
userland, not a container runtime or network appliance. Six groups, two
verdicts:

| # | group | examples | verdict | why |
|---|---|---|---|---|
| 1 | namespaces / mount / overlayfs | `unshare(CLONE_NEWUSER\|NEWNS)`, `mount`, `pivot_root` | **don't need** | only serves container/sandbox tools (Docker, LXC, `bwrap`, `systemd-nspawn`, `firejail`); the project's own install path already rejected user-ns+overlayfs as its mechanism (`design.md`) for the same kernel reason (no `CONFIG_USER_NS`). No package in the Alpha goal list needs this to run. |
| 2 | `swapon`/`swapoff` | swap management | **don't need** | only `util-linux`-class admin binaries call it; no compiler/language/CLI dev-tool touches it at runtime. |
| 3 | `mknod` | device node creation | **don't need** | Debian packages essentially never `mknod` at runtime (devices come from the kernel/udev already); not a compiler/language-toolchain concern. |
| 4 | SysV IPC | `shmget`/`semget`/`msgget` | **don't need (for now)** | legacy IPC, mainly old software / some DB / X11 client-server; nothing in the Alpha goal list depends on it. Revisit only if a future goal adds a database server or GUI/X target. |
| 5a | ports <1024 | binding a "well-known" port directly (80, 443, 22, ...) | **don't need** | plenty of unprivileged ports remain (>1024); a package that wants a low port can be given a high one instead (reverse-proxied or just reconfigured). Not a real limitation for a dev-tool userland -- nobody needs `deb-native` to bind :80 as itself. |
| 5b | raw socket (ICMP) / netlink | `ping`, `ip`/`iproute2` | **consider** | the one sub-case worth keeping: `ping`/`ip` are common early smoke tests ("does networking even work in the prefix"), not niche. Nuance not yet checked: netlink *reads* (route/interface listing) typically need no special privilege on stock Linux either, only netlink *writes* need `CAP_NET_ADMIN` -- so "netlink blocked" may be overstated pending an actual test. |
| 6 | `io_uring*` | modern async I/O (newer `nginx`, some DBs) | **deferred, not a goal** | confirmed absent from both seccomp allowlist files (Phase 1), so it's a real gap, but the project's target is a **dev-tool/CLI userland, not an HPC/high-throughput-server platform** -- this is explicitly a later add-on if a future survey sample ever turns up a package that needs it, not something to chase now. |

**Net effect on Phase 3:** groups 1-4, 5a and 6 need no on-device testing
right now -- recorded here as "out of scope, deliberately" (1-4, 5a) or
"deferred" (6), the same treatment `shim-coverage.md` already gave
`mount`/`umount2`/`chroot` (measured 0-1 occurrences in the 258-package
sample). Phase 3's actual on-device work narrows to **group 5b only**:
verify what specifically fails for `ping`/`ip`, and whether netlink reads
really are blocked or that claim is stale.

## Phase 3: group 5b tested on-device (2026-09-30, fe2, Termux app uid)

Ran directly, no corpus/download needed -- just `ping` and a small C
probe (`cc` is available in Termux):

- **`ping -c2 8.8.8.8` -- works.** Real ICMP replies, exit 0. Android's
  app sandbox permits unprivileged ping (no raw socket, no
  `CAP_NET_RAW`needed from this uid) -- not a limitation at all.
  `ping_group_range` itself is unreadable (`Permission denied` on
  `/proc/sys/net/ipv4/ping_group_range`) but whatever the effective
  policy is, it already allows it.
- **Netlink (`ip`/`iproute2`-class reads) -- blocked, and not by seccomp
  or capability.** A minimal C probe:
  `socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)` **succeeds** (passes the
  seccomp allowlist fine -- `socket()` itself is generic), but
  `bind()` on that fd fails **`EPERM`**, before a single `RTM_GETLINK`
  dump request could even be sent. On stock Linux, binding a netlink
  socket with no multicast groups requested doesn't need
  `CAP_NET_ADMIN` -- only joining certain multicast groups does. Getting
  `EPERM` on a plain bind, in a domain where `CapEff` is already `0`
  either way, points at **SELinux** (`untrusted_app_27`'s policy denying
  netlink sockets outright), not a capability or seccomp check.

**This adds a third, previously uncounted gate.** Revise the framing from
Phase 1: it's not just Gate A (seccomp) and Gate B (capability/kernel
config) -- there is also **Gate C: SELinux**, enforced independently of
both (confirmed here: the syscall passed seccomp, no capability was
needed for what was requested, and it still failed). `findings.md`'s
probe table already noted "SELinux is enforcing" for both domains but had
not pinned a concrete syscall to it before this test.

**Conclusion for group 5b:** `ping` needs nothing further -- already
works. `ip`/`iproute2` and anything else opening an `AF_NETLINK` socket
will not work in the Termux app domain, full stop; this is a policy
denial, not a missing patch, and neither own-glibc nor the tracer can
route around SELinux the way the tracer's SIGSYS trick routes around a
seccomp kill. Record as **out of scope**, same bucket as groups 1-4, but
for a different underlying reason -- worth keeping distinct in case a
future SELinux-adjacent finding needs the same "which gate" question
asked again.

## Phase 3 (original plan, superseded by the direct test above)

Goal: turn the Phase-1 allowlist into a real allow/deny table for this
device's actual arch (arm64) and kernel (5.10.240), and classify every
`runtime-failures.md` entry into Gate A / Gate B / neither, using the
existing probe method (`../spec/android-platform.md`, "Device probe:
sandbox limits confirmed directly") --
Termux app uid (`untrusted_app_27`) and `dsh` shell uid (`u:r:shell:s0`),
since comparing the two separates "kernel-wide" from "app-seccomp-only."
Turned out unnecessary for group 5b (a direct test settled it in two
commands); kept here in case group 6 (`io_uring`) or a future group ever
needs the fuller allow/deny table.

Plan (not yet run):

1. Fetch `bionic/libc/SYSCALLS.TXT` for the full bionic-exposed set, diff
   against the two allowlist TXT files above to get the arm64 allowlist as
   actually assembled (not just the two app-specific files quoted in
   Phase 1).
2. Build a small static-binary harness (reuse `tracer/`'s test pattern,
   `tests/tracer-nss/run.sh` as a template) that calls each candidate
   syscall directly (raw `syscall(nr, ...)`, no libc wrapper, so bionic's
   own allowed-by-construction wrappers don't mask the filter) and records
   the result.
3. Run it twice: once under Termux (`untrusted_app_27`, filter active),
   once under `dsh` (`u:r:shell:s0`, `Seccomp: 0`). A syscall that fails
   in both is Gate B / kernel-wide; one that only fails under Termux is
   Gate A (app seccomp specifically).
4. Prioritize the `runtime-failures.md` E list first (SysV IPC, `mknod`,
   raw sockets, netlink) since those rows are still "not yet
   distinguished" in the Phase 2 table above.

## Phase 4 idea, not decided (2026-09-30): fake the Gate-B failures too?

Raised in discussion: since the project already fakes identity for
fake-root (0.3.0, `getuid`/`stat` owner -> 0, no real right gained,
nothing recorded -- we're already building our own sandbox illusion, one
layer), could the same trick extend to some Gate-B failures -- answer
`mount`/`unshare`/`swapon`/... with a faked success instead of `EPERM`,
the same way `dn-trace`'s SIGSYS emulation already fakes `set_robust_list`
succeeding?

**Not the same risk profile as fake-root, and needs a per-syscall call,
not a blanket policy:**

- **Safe to fake (no-op, nothing relies on it for real isolation):**
  `swapon`/`swapoff` (program just wanted more memory headroom; faking
  success and doing nothing changes nothing observable), probably
  `mknod` for device files nothing reads from. Faking these is in the
  same spirit as fake-root: no real right gained, no one is misled into a
  false sense of safety.
- **Dangerous to fake:** `mount`, `unshare(CLONE_NEWUSER/NEWNS)`, anything
  a program uses **to sandbox something else** (spawn an untrusted child
  in a namespace, `chroot` an unprivileged worker) rather than just to
  get a resource. Faking success here doesn't just fail silently -- it
  tells the caller isolation happened when it didn't, which is worse than
  a clean `EPERM`: the program proceeds believing a security boundary
  exists. This is a different failure mode from fake-root (which fakes an
  *identity*, not a *guarantee*) and should not be treated as the same
  kind of trick.

Before implementing any of this: classify each Gate-B syscall by whether
real programs in the survey sample use it for resource acquisition (fake
it) or for isolation/sandboxing (don't -- let it fail honestly, same as
today). Not started; depends on Phase 3's syscall list existing first.

## Phase 5 idea, not decided (2026-09-30): clean-failure translation (Gate A only, no faking)

A third option, distinct from both "fake success" (Phase 4, risky for
isolation-sensitive calls) and "let it crash": for **Gate A only**
(syscall absent from the seccomp allowlist), translate the kernel's
`SIGSYS` process-kill into a clean, catchable **`ENOSYS`** instead --
never a fake success, just an honest "not available here" in the same
vocabulary every portable program already speaks (a kernel without
`io_uring`, or without `clone3`, already returns `ENOSYS` for it; nothing
is being lied about).

**Why Gate B/C need none of this, and why that matters:** every Gate-B/C
failure already tested returns a normal, catchable errno --
`unshare(CLONE_NEWUSER)` -> `EINVAL`, `mount`/`CLONE_NEWNS` -> `EPERM`,
the netlink `bind()` (Gate C) -> `EPERM`. This is indistinguishable from
what an unprivileged user gets on real Debian hardware. Well-written
software already handles it. **No project code needs to touch Gate B/C
failures at all** -- they're already "clean," which is exactly the goal
stated for this idea. Only Gate A is the anomaly: it doesn't return an
errno, it kills the whole process via `SIGSYS`, which is not something
any portable program's error handling expects or can catch.

**No library patch needed, own-glibc is irrelevant here:** the syscall a
program issues is identical whether it came from Debian's own glibc
(0.5.0) or Termux's current one -- what happens to it (kill vs. clean
errno) is decided entirely at the kernel/tracer boundary, after the
syscall has already left the library. This is purely `tracer/tracee/
seccomp.c`'s job: it already catches `SIGSYS` and substitutes a return
value for `set_robust_list` (currently a fake success, harmless because
that call is best-effort/informational). Extending the same mechanism to
answer other Gate-A syscalls (`io_uring_setup`/`_enter`/`_register`, and
whatever else Phase 3's fuller allow/deny table eventually names) with
`ENOSYS` instead of a fake success is a tracer-only change: a bigger
lookup table in one file, nothing upstream of it.

**The catch, same boundary as everywhere else in this doc:** the
`SIGSYS`-catch only fires for a process already running *under*
`dn-trace` (`ptrace` attached) -- the fast shim-only path for ordinary
dynamic glibc binaries has no tracer attached, so a Gate-A syscall there
still kills the process raw, exactly as today. Giving a program "clean
ENOSYS instead of crash" for a Gate-A syscall requires that program to be
routed through the tracer in the first place -- back to `dn-run.c`'s
`classify()` and the same routing question already open in the fake-root
section above. Independent of and complementary to
`SECCOMP_RET_USER_NOTIF` (angle 1) and own-glibc (0.5.0); not started.

## Phase 6 decision (2026-09-30): narrow the tracer, give the shim its own SIGSYS fallback

Discussion decided the tracer's scope directly (not left as an open question
like Phase 4/5 above): **`dn-trace` stays only a tool for static binaries and
raw-syscall programs (boundary cases 2/4/5), with its `SIGSYS` emulation kept
as a fallback *inside that same scope*** -- not grown into a general-purpose
router for every dynamic glibc program. That scoping immediately raises a
symmetry question: if traced programs get a clean-death fallback, the
shim-only path (every ordinary dynamic glibc binary, no tracer attached --
the vast majority of what runs) has none today, and a Gate-A syscall there
still raw-kills the process exactly as Phase 5 already described.

**The fix does not need `ptrace` or growing tracer routing.** The `SIGSYS`
Android's seccomp filter raises on a disallowed syscall is a real signal
delivered *into the very process that made the call* -- `dn-trace` only sees
it today because it happens to be attached via `ptrace`from the outside. A
process can catch its own `SIGSYS` with an ordinary `sigaction(SIGSYS, ...,
SA_SIGINFO)` handler: the kernel's `ucontext_t` gives the handler the
trapped syscall number and arguments (arm64: `uc_mcontext.regs[8]` and
`regs[0..5]`), the handler can write a faked result into `regs[0]`, advance
`uc_mcontext.pc` by 4 to skip the trapping `svc #0`, and return -- no
`ptrace`, no external process, same lookup table Phase 5 already sketched
(safe only for the no-op/best-effort Gate-A syscalls, never for anything
Gate B/C already answers with a clean errno).

**Where to install it: `native/ld-dn.c`, not `path-redirect.so`'s
constructor.** Checked the actual code and corrected a wrong assumption
along the way -- the shim is *not* embedded in `ld-dn`; they are two
separate build artifacts (`native/ld-dn.c` compiles to a standalone
freestanding ELF, `native/path-redirect.c` compiles separately to
`path-redirect.so` via `scripts/bootstrap/build-path-redirect.sh`). `ld-dn` only
*writes* `LD_PRELOAD=.../path-redirect.so` into the new environment/stack it
hands to glibc's real loader (`ld-dn.c:145-146`) -- it never maps the shim
itself. The real sequence for a translated Debian program:

    kernel execs ld-dn (PT_INTERP, freestanding, raw syscalls only, no glibc yet)
      -> ld-dn maps glibc's real loader and jumps into it (no re-exec, same process)
      -> glibc's loader resolves LD_PRELOAD, dlopens path-redirect.so, runs its constructor
      -> main()

A `sigaction(SIGSYS, ...)` installed from `path-redirect.so`'s constructor
would leave a real gap: **everything glibc's own loader does before that
constructor runs** (symbol resolution, `mmap`ing shared objects, reading
`ld.so.cache`, ...) is unprotected, since nothing has installed the handler
yet at that point. `ld-dn` runs *before all of that* -- before glibc is even
mapped -- and it is already freestanding/raw-syscall code (`sys6()` in
`ld-dn.c`), so it is the one place a `rt_sigaction` call can be added that
covers the *entire* process lifetime from the first instruction, including
glibc's own startup. `sigaction` state is per-process and survives `ld-dn`'s
jump into the real loader (not an `execve`, just a `br` in the same
process), so installing it once in `ld-dn` before the jump is sufficient --
no re-installation needed later.

**Net design:** the tracer's `SIGSYS` emulation and this new `ld-dn` handler
are two instances of the *same* clean-death policy (same candidate syscall
table: `set_robust_list`/`get_robust_list`, `io_uring_*`, SysV IPC, ...),
applied at the two different points a program can reach a Gate-A wall --
traced (static/raw-syscall) or shim-only (dynamic glibc, the common case).
Neither grows into the other's territory: `dn-trace`'s routing stays exactly
as narrow as decided above, and the shim gains its own fallback without
needing `ptrace` at all.

**Not started; not yet a full plan.** Open before implementation: the exact
freestanding `rt_sigaction`/`sigreturn` sequence in `ld-dn.c`'s style (no
libc), the shared candidate-syscall table's home (a header both `ld-dn.c`
and `tracer/syscall/exit.c` include, to avoid the two lists drifting apart),
and on-device verification that Android's filter action for a Gate-A
syscall is actually a catchable `SIGSYS` delivery (not `RET_KILL_PROCESS`,
which would deliver nothing to catch) -- assumed from this doc's own
wording ("historically SIGSYS/crash") and from `dn-trace`'s own working
`SIGSYS` emulation, but not independently confirmed for the *un-traced*
case specifically.

## Conclusion (2026-09-30)

**Do not scope 0.5.0 as "fixes what Android breaks" broadly.** Own-glibc's
confirmed leverage is the NSS/loader-internal-path class (same mechanism,
several symptoms: NSS, `gconv`, locale, `ld.so.cache`, `RUNPATH`) plus
whatever specific syscall stock Debian `libc6` trips at startup (still
unnamed -- the one open item below).

The relevance triage (Phase 2b) closed the rest of the investigation
cheaply: of the six candidate groups, four don't matter to this project's
actual goal (namespaces/mount, swap, `mknod`, SysV IPC) and a fifth's
worst sub-case doesn't either (ports <1024). The one real open question
(group 5b, `ping`/`ip`) was tested directly on-device in minutes --
`ping` already works, `ip`-class netlink is blocked by **SELinux**, a
third gate distinct from seccomp (A) and capability/kernel-config (B),
confirmed by a syscall that passed A and needed nothing from B yet still
failed. `io_uring` (group 6) is a confirmed real gap but deliberately
deferred, not a current goal.

**Net scope for three gates, now named with evidence each** — table moved to [`../spec/android-platform.md`](../spec/android-platform.md) ("The three enforcement gates"), updated with Gate C (SELinux, confirmed just above).


**Closed 2026-09-30 (was "still open" above): stock Debian `libc6` doesn't
even reach a syscall question.** Tested directly: downloaded the real
`libc6_2.41-12+deb13u4_arm64.deb` from `deb.debian.org`, ran it through
the project's actual translation pipeline
(`scripts/install/dn-translate-deb.sh`, against a live `~/.dn` prefix) to get a
faithfully-`patchelf`'d `libc.so.6`/`ld-linux-aarch64.so.1` (confirmed
`PT_INTERP`/`RUNPATH` correctly repointed into the prefix), then ran the
minimal `hello` package's binary (the same one `findings.md` already uses
as a test case) through it directly with `--library-path`.

**Result: segfault, before `LD_DEBUG=all` could write a single line** --
the crash is inside the dynamic linker's own startup, earlier than any
meaningful syscall a seccomp/capability/SELinux gate could even judge.
Control test with the identical `hello` binary through **Termux's**
`ld-linux-aarch64.so.1`/`libc.so.6` instead: runs clean, "Hello, world!",
exit 0.

**This settles the direction-3 question from the discussion above, for
real Debian glibc specifically: dead, and not just because of the
string-length problem found earlier** (Debian's stock strings are plain
`/etc/x`, shorter than this project's prefix path, so an in-place byte
patch doesn't even fit). Stock Debian's dynamic linker cannot get through
its own startup on this device *at all* -- patching NSS/sysconfdir
strings in it is moot, since execution never reaches NSS. This confirms
`termux-pacman/glibc-packages`'s Android patch series is load-bearing at
the loader-startup level, not merely a path-configuration convenience --
0.5.0 (rebuild-from-source) cannot skip or lighten that patch series.

**Direction 3 survives only in its already-corrected form: applied to a
private vendored copy of Termux's own glibc** (already carries whatever
patches make the `hello` control test pass), never to a fresh Debian
`.deb`. That copy's `/etc/...` strings are the long, Termux-prefixed kind
confirmed patchable in place (`FITS`, tested earlier) -- this remains the
one cost-effective NSS fix left standing, distinct from and much cheaper
than full source-rebuild 0.5.0.

**Cross-checked, not contaminated by a later finding:** a separate bug
found the same day (`findings.md`, "patchelf corrupting an `ET_EXEC`
binary's program headers") showed `dn-translate-deb.sh`'s old
`--set-rpath` step could corrupt an `ET_EXEC` binary into an identically-
shaped crash (segfault, no syscall in flight, inside loader startup).
Re-ran this section's own test material (`libc.so.6`,
`ld-linux-aarch64.so.1`, the `hello` binary -- all `ET_DYN`) through the
pipeline as it stood at the time: no corruption, clean program headers.
The bug is `ET_EXEC`-only; this section's conclusion stands.

## Termux's actual Android patch series, and the per-file fork verdict

Extracted as a standing reference once the full read-through below settled: see [`../spec/android-platform.md`](../spec/android-platform.md), "Termux's Android glibc patch: catalog and fork verdict" for the patch catalog and the per-file verdict for all 54 files in `gpkg/glibc/`. The reading pass itself, and the reasoning behind each verdict, happened in this session (2026-09-30) following directly from the Conclusion above.

## 0.5.0 first build attempt: partial fork tested, confirmed insufficient (2026-09-30)

A separate session (commits `9e70620`..`538555d`, `dev-0.2.0`) acted on
direction 2 before the open questions above were settled: forked and
retargeted only **`set-dirs.patch` + `disable-clone3.patch`** from
`termux-pacman/glibc-packages` onto Debian's real glibc `2.41-12+deb13u4`
source, built it via a new `.github/workflows/build-glibc.yml` (on-device
build blocked separately -- `clang` can't build glibc, Debian's own
`gcc-14` installed through this project's own apt segfaults on `cc1`, an
unrelated pipeline bug). CI ran 9 times (`gh run list`); the two latest
report "success", but that status is `make -k install` tolerating an
`Error 2` in the install step to still package an artifact -- not a clean
build. Also left an untracked 176 KB `core` file in the working tree,
presumably from the local `gcc-14`/`cc1` SIGSEGV mentioned in the commit
message.

**Tested here, for real, against the actual artifact** (`gh run download`
the latest success, `dn-glibc-android-538555d....tar.gz`): confirms both
the good part and the gap.

- **The NSS path retarget worked**: `strings` on the built `libc.so.6`
  shows `/data/data/com.termux/files/home/.dn/etc/passwd` and
  `.../nsswitch.conf` -- `set-dirs.patch`'s `@TERMUX_PREFIX@` placeholder
  correctly substituted to this project's own prefix path, not Termux's.
- **Still does not run.** Same `hello` control test used earlier in this
  doc (against stock Debian glibc, which segfaulted immediately): this
  build's `ld-linux-aarch64.so.1` + `libc.so.6`, via `--library-path`,
  dies with **signal 31 (`SIGSYS`)** -- progress over stock Debian's
  instant segfault (Gate-A kill happens later, not a loader-level crash),
  but still not working. `LD_DEBUG=all` shows it getting through vDSO
  symbol resolution (`__kernel_clock_gettime`/`_gettimeofday`/`_getres`
  all bind normally) and into `libc.so.6`'s own load + `GLIBC_2.17`/
  `2.34`/`2.35`/`2.38`/`GLIBC_PRIVATE` version checks, then the debug log
  simply stops mid-stream with no further output -- consistent with an
  unannounced `SIGSYS` kill, not narrowed to the exact syscall yet.
- **Expected, matches this doc's own findings above, not a surprise.**
  `set-dirs.patch` alone was never claimed to be sufficient -- the patch
  catalog (`../spec/android-platform.md`) already named
  `fakesyscall.json` (the `disabled-syscall.h` mechanism, wired into
  glibc's build via several `.c` files: `mprotect.c`, `syscall.c`,
  `setfsuid.c`/`setfsgid.c`, `fake_epoll_pwait2.c`, the `syscall.S`/
  `disabled-syscall.h` codegen loop in `build.sh`) as the piece that
  actually keeps glibc's own startup off Android's seccomp-killed
  syscalls. `disable-clone3.patch` alone removes one specific offender;
  it was never going to be the only one glibc's startup path hits.

**Conclusion: the partial-fork approach is on the right track (NSS retarget
proven) but incomplete -- forking `fakesyscall.json` and its supporting
`.c` files is not optional groundwork, it's required for the build to
survive its own startup**, not just a refinement for later. Next step
before another CI cycle: narrow down which exact syscall in `libc.so.6`'s
early startup trips `SIGSYS` (needs `dn-trace` or a `sigaction(SIGSYS,
SA_SIGINFO)` probe wrapper around the `hello` test, since `strace`/`ltrace`
are unavailable on-device) and confirm it's on `fakesyscall.json`'s known
list before assuming forking that file fixes it outright.

## Everything marked "fork" actually forked (2026-09-30)

Done, onto `~/dn-glibc-build/work` (Debian's real glibc `2.41-12+deb13u4`
source + Debian's own quilt series + this project's retargeted patch):
every file/hunk marked "fork" in the table above, minus the
`set-ld-variables.patch` question (left for its own read) and the
fake-root-parked bucket-3 pieces (`"0"`-bucket `fakesyscall.json` entries,
`setfsuid.c`/`setfsgid.c`, the `set*id.c`/`local-setxid.h` hunks of
`set-fakesyscalls.patch` -- split out with `csplit` into
`set-fakesyscalls-forked.patch`/`-parked.patch` and only the former
applied). One patch (`disable-termios2.patch`) turned out to target glibc
internals (`sysdeps/unix/sysv/linux/isatty.c`, a `termios2` struct) that no
longer exist in this shape in 2.41 -- `isatty.c`/`isatty_nostatus.c`/
`termios_internals.h` aren't even at those paths anymore, and
`__ASSUME_TERMIOS2` is gone from `kernel-features.h` entirely; glibc
restructured termios handling since whatever version this patch was
written against.

**Resolved 2026-09-30, empirically: not needed at all, not just hard to
port.** `grep -rl 'TCGETS2\|struct termios2\|__ASSUME_TERMIOS2'
sysdeps/` across the whole clean-room `work-clean` tree returns nothing --
`termios2` doesn't exist anywhere in 2.41's source, not just at the paths
this patch expected. `tcgetattr.c`/`tcsetattr.c`/`isatty.c` already use
plain `TCGETS`/`TCSETS` against `struct __kernel_termios` unconditionally,
exactly the behavior `disable-termios2.patch` was trying to force onto an
older glibc. Confirmed live, not just by reading source: a test program
(`posix_openpt`/`grantpt`/`unlockpt` to get a real pty, then
`isatty`/`tcgetattr`/`cfsetispeed`+`cfsetospeed`(`B9600`)/`tcsetattr`/
re-`tcgetattr` to verify the round-trip) run through `dn-trace -v 4`
against the clean-room `libc.so.6`/`ld.so` shows every terminal `ioctl`
using `0x5401`/`0x5402` (`TCGETS`/`TCSETS`) -- never `TCGETS2`/`TCSETS2`
(`0x802c542a`/`0x402c542b` on aarch64) -- and the baud-rate round-trip
comes back correct. Terminal I/O works without this patch, full stop; the
"needs a real port" item above is closed as not applicable to 2.41, not
left open.

One patch (`set-static-stubs.patch`) had one hunk fail on context drift
(2 leftover `link ()` call sites the same patch's own earlier hunk had
already renamed to `link_unwind ()` elsewhere in the file, plus a missing
`#if !HAVE_GCC_PERSONALITY_V0` guard) -- both hand-fixed identically to
what the rejected hunk asked for, confirmed by reading the `.rej` file
against the patch's own intent, not guessed.

`disabled-syscall.h` generated for `aarch64` only (this project's only
target) using `build.sh`'s own `jq`-driven codegen, against
`fakesyscall.json` with the `"0"` key removed
(`jq 'del(.["0"])' fakesyscall.json`) -- several bucket entries
(`fchownat`->`chown`/`chown32`, `getpgrp`, `recvfrom`->`recv`, `symlink`,
`link`, `unlinkat`->`rmdir`) produced no `case` at all, because `aarch64`'s
own `arch-syscall.h` never defines separate `__NR_chown32`/`__NR_recv`/...
in the first place -- consistent with `kernel-features.h.patch`
(forked in the same pass) already saying this arch has no separate
`accept`/`recv`/`send` syscalls to disable.

## First successful self-built `libc.so.6`/`ld.so`, and `hello` runs through it (2026-09-30)

After the `clone3.S` fix above, one more real snag: the build got
**`Terminated`** mid-compile (`math/e_jnf.o`), not from any bug in this
project's patches -- Android reclaiming a backgrounded Termux session's
child processes. Fixed with `termux-wake-lock` before restarting; no
further terminations.

**`elf/ld.so` and `libc.so`/`libc.so.6` built clean** -- the `make -k`
rerun (idempotent: only the `clone3`-cascade targets and downstream
`others`/`install` work were still outstanding) finished with **zero**
`Error`/`Terminated` lines. First real test, the same `hello` control
binary used throughout this doc, run directly against the freshly-built
pair (no install step, no shim, no `ld-dn` -- just the raw output of this
project's own build):

```
$ objdir/elf/ld.so --library-path objdir pristine/usr/bin/hello
Hello, world!
$ echo $?
0
```

**This is the first glibc this project has ever built from source that
actually runs a real Debian binary to completion.** The CI-built artifact
from earlier today (`538555d`, only `set-dirs.patch` +
`disable-clone3.patch`) died with `SIGSYS` at this exact step; forking
`fakesyscall.json`'s two real buckets (substitute + honest `ENOSYS`) is
what closed that gap, exactly as predicted when that CI result was first
analyzed above ("forking `fakesyscall.json` ... is not optional
groundwork, it's required for the build to survive its own startup").

NSS verification (the actual point of 0.5.0, `set-dirs.patch`'s
`@TERMUX_PREFIX@` retarget) needs a `getpwnam`-style test program compiled
against this new libc -- attempted directly against the raw source tree's
headers (`work/include`, `work/posix`, ...) and failed with cascading
parse errors (`bits/types.h`'s `__int32_t` etc. -- these are generated/
finalized by `make install-headers`, not usable straight from the
unconfigured source tree). `make -k install DESTDIR=.../destdir` used
instead, for a real, self-consistent installed header+library set.

**Install finished clean too** (all of `ld.so`, `libc.so.6`, `sprof`,
`pldd`, `ldd`, `sotruss`, `sln`, `ldconfig`, headers, `stubs.h`
installed) -- the only hiccup was the very last step, `ldconfig -r` run
directly by the `Makefile` itself (not through this project's own
pipeline) to refresh the destdir's `ld.so.cache`: **`Unknown signal 31`**
(`SIGSYS`) -- expected, this is a self-built binary run raw, outside
`ld-dn`/the tracer, hitting some Gate-A syscall not in the forked
`ENOSYS`/substitute buckets. `make` reported it "(ignored)" and the
install completed regardless -- same tolerance shape as `build-glibc.yml`
already has for the `manual/` subdirectory.

**NSS confirmed working, against the installed tree, with real headers
this time.** Compiled a `getpwnam`/`getpwuid` test program against
`destdir`'s installed `usr/include`, dynamically linked against the
freshly-built `libc.so`/`ld.so` (`-Wl,--dynamic-linker=.../elf/ld.so`),
run with no shim, no tracer, no `ld-dn` -- just this project's own build,
directly:

```
$ elf/ld.so --library-path objdir t3
getpwnam(root) FOUND uid=0 dir=/root shell=/bin/bash
getpwuid(0) FOUND name=root
```

Matches the real prefix's actual `/etc/passwd` entry exactly
(`root:*:0:0:root:/root:/bin/bash`) -- read directly from
`/data/data/com.termux/files/home/.dn/etc/passwd` via `set-dirs.patch`'s
retargeted path, confirmed earlier by `strings` on `libc.so.6`. **This is
0.5.0's core goal, achieved**: NSS resolves through the prefix's own
`/etc` with no tracer route, no `/etc` bind, no workaround -- the thing
`shim-coverage.md` and `syscall-boundary.md` both concluded was
structurally unreachable at the libc-interposition layer is now just...
how this glibc behaves, because it was built that way.

**Not yet done, to turn this into something the rest of the project can
actually use**: package as `libc6`/`libc6-dev`/`libc-bin` `.deb`s,
version them like Debian's own (`2.41-12+deb13u4+dn1`), replace
`dn-adopt.sh`'s current symlink-to-whatever-Termux-has with these,
re-verify `cc1`'s own segfault fix and the `mkdir`/`PATH` finding survive
a second from-scratch build (this run reused an already-forked `work/`
tree with several hand-fixes applied mid-build -- clean-room replay not
yet done), and root-cause the `ldconfig`/`SIGSYS` gap properly (Gate-A
clean-death handler, `android-seccomp-audit.md` Phase 6, still "not
started") rather than relying on `make -k` to paper over it.
Regenerated `dn-glibc-android.patch` (`diff -ruN --exclude=.pc
--exclude=debian pristine/p work`, 147 file-diffs, up from 67) and
round-trip-verified: fresh copy of `pristine`, apply the regenerated
patch, diff against `work` -- empty. Landed in
[`third_party/glibc-android-patches/`](../../third_party/glibc-android-patches/)
(`dn-glibc-android.patch` + `README.md`, updated with the full
per-file breakdown and a documented regeneration recipe). Not yet built
or tested on-device -- that's the next step, either via CI
(`build-glibc.yml`, unchanged, `patch -p1` handles the new-file hunks the
same way) or on-device now that `cc1`'s `ET_EXEC` segfault (`findings.md`,
2026-09-30) no longer blocks a native build attempt.

## First real on-device `configure`/`make` attempt (2026-09-30)

Tried the on-device path right away, `~/dn-glibc-build/work` (the fully
forked tree above) with `CC=$DN/usr/bin/gcc-14`.

**`configure` succeeded outright** -- reached `config.status`/`Makefile`/
`config.h`, including `checking for redirection of built-in functions...
yes`, the exact check that fails `clang` unconditionally (`TODO.md`'s old
build-step blocker). Direct confirmation the `cc1`/`ET_EXEC` fix
(`findings.md`, 2026-09-30) actually unblocks a real, on-device glibc
`configure`, not just a standalone `cc1 -v`/`gcc -S` smoke test.

One real gap on the way there: **kernel UAPI headers**
(`--with-headers`) aren't part of this project's base bootstrap or
anything `dn-translate-deb.sh` touches -- `configure` fails outright
without them ("GNU libc requires kernel header files"). Debian's
`linux-libc-dev` package provides them normally; **now installed
properly through the prefix's own apt** (`linux-libc-dev:arm64
6.12.111-1`, confirmed `dpkg -s`) rather than left as the ad-hoc
downloaded-and-merged scratch directory used to unblock this specific
`configure` run. The multiarch layout needs care either way: Debian's
`.deb` ships `usr/include/linux`, `usr/include/asm-generic` and the
*real* `asm/` under `usr/include/aarch64-linux-gnu/asm` (a target-triplet
subdir, not a top-level `usr/include/asm`) -- glibc's own build passes
`-nostdinc` plus an explicit `--with-headers=DIR` merging all three into
one flat directory, so it doesn't get gcc's own automatic multiarch
search the way an ordinary compile would. A first attempt at that merge
copied the `.deb`'s symlinks as-is (`cp -r`) -- `usr/include/
aarch64-linux-gnu/asm/errno.h` is a *relative* symlink
(`../../../lib/linux/uapi/arm64/asm/errno.h`) that only resolves from
inside the original package tree, so copying it elsewhere without
dereferencing breaks it silently until something includes
`<asm/errno.h>`. Fixed with `cp -rL` (dereference, copy real content).

**Then hit a real, reproducible instability during `make`**, not the
kernel-header issue: intermittent `Segmentation fault` on totally
unrelated recipes (`minihelp`, a bare `@echo`; `nscd`'s `stamp.os`; a
plain `mkdir`), misattributed to whatever recipe `make` happened to be
running when a stray SIGSEGV notification arrived -- confirmed by
checking actual output: the "failed" `sysd-syscallsT` file this
produced was in fact complete and correct, and running the exact same
`make-syscalls.sh`/`gcc-14 -E` invocations by hand (40x in a loop) never
crashed once. **Root cause: `PATH="$DN/usr/bin:$PATH"`** (set to help
`gcc-14` find the prefix's binutils, before realizing `native/ld-dn.c`'s
`COMPILER_PATH` fix from earlier today already makes that unnecessary)
put this project's own glibc coreutils **ahead of Termux's own** --
`mkinstalldirs`'s plain `mkdir` calls, and other trivial utility
invocations throughout glibc's `Makefile`s, silently resolved to
`$DN/usr/bin/mkdir` (a real glibc ELF, going through the full
`ld-dn`+shim+loader dance) instead of Termux's lightweight Bionic
`mkdir` -- hundreds of times over the course of a build, evidently not
yet stable enough for that volume/rate of invocation. Fix: don't put
the prefix on `PATH` at all -- `COMPILER_PATH` (already set
automatically per-launch by `ld-dn`) is sufficient for `gcc-14` to find
its own subprograms, so plain `PATH=/data/data/com.termux/files/usr/bin`
(Termux's own tools only) is both correct and sufficient. Zero
segfaults observed since.

**Open, not yet root-caused**: *why* rapid, repeated invocation of a
`ld-dn`-routed program is less stable than Termux's own binaries under
this kind of sustained load -- parked as a real question (this project's
own coreutils will need to run this way eventually, just not while
avoidable via `COMPILER_PATH`/`PATH` choices), not investigated further
here since the immediate build no longer needs it.

**One real (non-flaky) compile error found and fixed once the segfault
noise was gone**: `misc/clone3.S` (the raw syscall stub behind the
*public* `clone3()` glibc function -- distinct from `clone-internal.c`'s
internal usage, which `disable-clone3.patch` already handles) failed to
assemble, `undefined symbol __NR_clone3 used as an immediate value`.
Cause: `clone3` is in `fakesyscall.json`'s `ENOSYS` bucket (forked
today), and the `disabled-syscall.h` codegen deliberately deletes the
matching `#define __NR_clone3` out of `arch-syscall.h` so nothing can
reach the kernel's real (Gate-A-killed) syscall by accident -- but
`clone3.S`'s own raw wrapper still referenced it directly, outside the
fakesyscall dispatch path. This is exactly why `gpkg/glibc/build.sh`
itself does `rm sysdeps/unix/sysv/linux/*/clone3.S` unconditionally as
its very first step (`termux_step_pre_configure`) -- a step this
project's fork only partially carried over (the loose-file
`disable-clone3.patch`, not build.sh's own `rm`). Fixed the same way
upstream does: deleted `sysdeps/unix/sysv/linux/aarch64/clone3.S` from
the work tree (aarch64 only, this project's one target). A stale `.d`
dependency file from an earlier partial run (generated before the fork
was complete) still hardcoded the now-deleted path and caused one more
false "No rule to make target" failure after the fix was already
applied — deleting `objdir/misc/clone3.o{,s}{,.d}` let `make` regenerate
it and fall back correctly to the portable `sysdeps/unix/sysv/linux/clone3.c`.
Needs folding back into `dn-glibc-android.patch` (as a deleted-file diff
hunk), done below.

## First full on-device build: green, and it runs (2026-09-30)

**`make` completed with zero errors** (`objdir/libc.so.6` -> `libc.so`,
`objdir/elf/ld.so` built, 1.2 MB) after two false starts, both from this
build's own setup rather than the patch content: the `-k`/dependency-order
issue above, and the earlier `PATH`-shadowing `mkdir` instability
(`findings.md`). A straight (non-`-k`) `make -O -j1` run, once those two
were fixed, went start to finish without a single real error.

**Ran the `hello` control test this doc has used throughout** (the same
one that segfaulted instantly against stock Debian glibc, and `SIGSYS`'d
against the earlier partial-fork CI artifact): `elf/ld.so --library-path
objdir pristine/usr/bin/hello` →

```
Hello, world!
exit=0
```

**Confirmed both fixes landed, not just one:**
- NSS/path retarget: `strings objdir/libc.so` shows
  `/data/data/com.termux/files/home/.dn/etc/{mtab,fstab,hostid,ttys,shells,...}`
  — the project's real fixed prefix, not `@TERMUX_PREFIX@` or Termux's own path.
- Startup no longer trips a Gate-A `SIGSYS`: the `fakesyscall.json`
  ENOSYS/real-substitute buckets forked this session (`clone3`,
  `set_robust_list`, etc. now answered cleanly instead of reaching the
  kernel) evidently cover whatever stock `libc6`'s startup path was
  hitting — `hello` would have died the same way the CI artifact did
  otherwise.

This is the first time this project's own-glibc has actually **run** a
program, not just built.

## `make install`, and NSS confirmed functionally working, not just built-in (2026-09-30)

`make -k install DESTDIR=destdir` completed; every core artifact landed
(`usr/lib/libc.so.6`, `usr/lib/ld-linux-aarch64.so.1`, `usr/include/pwd.h`,
...). `-k` mattered here too: the manual (missing texinfo sources, same
gap `build-glibc.yml`'s own comment already documents) fails install for
its own subdir but nothing else is affected.

**One real, reproducible bug found along the way, not blocking**: `elf/ldconfig -r`
(rebuilding `ld.so.cache`) dies with **signal 31 (`SIGSYS`)** when run directly,
every time (3/3) -- but **succeeds cleanly (exit 0) every time under `dn-trace`**
(`dn-trace -v 4 -- ldconfig -r ...`, full syscall log, no error). Ptrace attachment
changing whether a syscall reaches Gate-A's kill action is a real, known
seccomp/ptrace interaction (not investigated further here -- which exact syscall,
and why the tracer avoids it, is open). Not blocking: `ld.so.cache` is a lookup
optimization, not required for the loader to function (confirmed next). Practical
options if this needs a real fix later: route `ldconfig` through `dn-trace` the
way static binaries already are, or find and add the specific syscall to
`fakesyscall.json`.

**NSS confirmed functionally correct, not just the right strings baked in.**
Compiled a small test program (`getpwuid(0)`) against the newly-installed
headers, linked manually against the destdir's own `libc.so.6`/`libc_nonshared.a`/
crt objects, ran it through the newly-built `ld.so --library-path destdir/lib`:

```
uid 0 -> root (home=/root shell=/bin/bash)
```

Matches the prefix's real `/etc/passwd` (`root:*:0:0:root:/root:/bin/bash`)
exactly -- NSS is genuinely resolving against this project's own `/etc`, at
runtime, not just carrying the right path as a dead string. This closes the
loop `set-dirs.patch` was forked for in the first place: the tracer's NSS
route (`../spec/shim-coverage.md`, "Resolved 2026-09-26") is no longer the only
way to get this -- own-glibc now does it natively, no tracer needed, for any
program linked against it.

**Net effect of this session's work**: on-device `configure` succeeds (`cc1`
fix), a full `make`/`make install` succeeds with zero real errors (`PATH`/
`clone3.S` fixes), the result runs (`hello`), and its defining feature works
end to end (NSS). 0.5.0's "what it needs" `proof:` checklist item (`gcc`
hello-world, NSS without the tracer) is now demonstrated, not just planned.

## Clean-room replay: patch alone, from a fresh `pristine` copy (2026-09-30)

Closed the "not yet done" item from the previous section: the build above
reused an already-forked `work/` tree with several hand-fixes applied
mid-build (`PATH`, `clone3.S`) that were never folded back into
`dn-glibc-android.patch`. Replayed from scratch to find out what a real
user following `README.md`'s own "Applying this patch" recipe would
actually hit: fresh `pristine` copy, `patch -p1` with the patch as it
stood in the repo, nothing else.

**Confirmed a real gap**: the `clone3.S` deletion was still missing from
the patch (only ever applied as a manual `rm` in `work/`) — a clean apply
of the patch alone reproduces the exact compile error from the first
build attempt (`undefined symbol __NR_clone3 used as an immediate value`).
Patch itself applied cleanly otherwise: 147 file-diffs, `.rej`-free, matches
the documented count exactly.

**Fixed properly this time**: deleted the file in the clean-room tree, then
regenerated `dn-glibc-android.patch` (`diff -ruN --exclude=.pc
--exclude=debian pristine work`, now 148 file-diffs) instead of leaving it
as an undocumented tree edit. Round-trip verified per `README.md`'s own
recipe (fresh `pristine`, apply the regenerated patch, diff against the
clean-room `work` tree — empty). This is now a real fix in the patch, not
a fact only true of one hand-edited tree.

**Also settled an open question from the `PATH`-shadowing finding**:
retried the full build at `-j8` (this device has 8 cores) instead of the
`-j1` that first got a clean run, to check whether the earlier segfault
instability was actually about parallelism or genuinely just the `PATH`
issue. `make -j8`, `make install`, and the `hello` control test all
succeeded with zero segfaults — confirms it was `PATH` alone; `-j8` is
safe and roughly 3-4x faster wall-clock, no reason to keep defaulting to
`-j1` for future builds on this device.

The only non-zero-exit output from `make -k install` was the
already-documented, non-blocking `ldconfig -r` `SIGSYS` (signal 31) gap
from the section above — unrelated to this session's changes, still open.

Net effect: `dn-glibc-android.patch`, as it now stands in the repo, is
sufficient on its own (`patch -p1` against a fresh Debian `glibc` source,
no manual tree edits) to reach a working `libc.so.6`/`ld.so` that runs
`hello` — the gap between "works in this one hand-fixed tree" and "works
by following the README" is now closed.

## Seccomp visibility for the post-install analysis pass, and `rseq` found (2026-10-01)

Groundwork for a design question in `TODO.md` ("Shim & tracer hardening"):
routing a "well-behaved" binary to the tracer when a public libc call's
*internal* syscall choice trips Gate A (the `ldconfig -r` case — no NSS
import, no raw syscall of its own, so neither of the two existing routing
scans flags it). Before deciding how to route such a binary automatically,
we need to actually see which syscalls a traced program hits that get
blocked — today that information exists but is discarded silently.

**Found**: `tracer/tracee/seccomp.c`'s SIGSYS handler already has a
generic `default:` case (`handle_seccomp_event_common()`) that answers
*any* syscall this device's seccomp policy blocks with a clean `-ENOSYS`,
whether or not the tracer's own syscall table (`sysnums-arm64.h`) has a
name for it — this was already correct and already covers more than the
one `set_robust_list` case `TODO.md`'s older phrasing named specifically.
What was missing was visibility: nothing logged which syscall it was, even
under `-v`.

**Fix**: that `default:` case now calls `note()` (prints at default
verbosity, not gated behind `-v`, unlike the existing `VERBOSE()` calls
around it) naming the syscall via the existing `stringify_sysnum()` plus
its raw number, before returning `ENOSYS`.

**Immediately found a real gap while testing this** (against
`busybox-static`'s `true`, already installed on the test device):
syscall **#293 has no entry in `sysnums-arm64.h` at all** — it printed as
unnamed ("void") even with the new logging. That number is **`rseq`**,
which glibc >= 2.35 registers unconditionally for *every* thread,
including the main one, at process startup. So this fires on essentially
every traced glibc/NPTL program's first moment, not some rare edge case.
It was already handled correctly before this session's change (falls to
the same clean-`ENOSYS` default; glibc's own `rseq` registration already
tolerates `ENOSYS` gracefully, unlike the fatal-`SIGSYS`-on-probe pattern
this whole investigation is about) — it was just invisible. Added a named
`[ 293 ] = PR_rseq` entry (`syscall/sysnums.list` + `sysnums-arm64.h`) so
the log names it instead of "void" going forward.

**Also caught and fixed a bug in the new logging itself while verifying
it**: the first attempt peeked the raw syscall number from the `ORIGINAL`
register snapshot while `sysnum` (used for the name) was resolved from
`CURRENT` a few lines above — inconsistent, and printed a stale, unrelated
number left over from an earlier syscall in the same seccomp batch
(`set_robust_list`'s, from immediately before). Caught by cross-checking
against `-v 4`'s full per-syscall trace, which prints `CURRENT` sysnum
consistently (`print_current_regs()`, `tracee/reg.c`) — fixed to also peek
`CURRENT`.

Verified end to end: `busybox-static true` now logs exactly
`blocked syscall rseq (#293) denied by seccomp; returning ENOSYS` and
still exits 0; a full `apt-get install` and `find -exec test` regression
pass with zero `logcat -b crash` entries after rebuilding and redeploying
`dn-trace`.

**Scope note**: this is visibility only, not the routing decision. Which
mechanism actually gets an `ldconfig -r`-shaped binary *into* the tracer
in the first place — reactive retry-and-cache vs. a one-time audit of
glibc's own probe-and-fallback call sites — is still open, deliberately
not attempted here (`TODO.md`). Next step for the analysis pass itself:
run this logging against a real package's actual binaries, not just a
synthetic probe, to see how common blocked-but-tolerated syscalls other
than `rseq`/`set_robust_list` actually are before choosing between those
two shapes.

