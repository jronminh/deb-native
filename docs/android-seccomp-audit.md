# Android seccomp/capability audit (started 2026-09-30)

Why: `docs/runtime-failures.md` and `docs/findings.md` list several things
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

## Phase 1: what AOSP's source says (done, 2026-09-30)

Two independent gates exist, and a syscall can fail either one:

**Gate A -- seccomp-bpf filter, installed by zygote into every app process**
(`Seccomp: 2`, confirmed by direct probe in `findings.md`). Default-deny:
a syscall not on the allowlist is refused (historically SIGSYS/crash --
"Android O crashes an app that uses an illegal syscall", see sources).
The allowlist is assembled from:

- [`bionic/libc/SYSCALLS.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SYSCALLS.TXT)
  -- every syscall bionic itself exposes a wrapper for.
- [`bionic/libc/SECCOMP_ALLOWLIST_APP.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SECCOMP_ALLOWLIST_APP.TXT)
  -- app-process additions. Confirmed present (fetched 2026-09-30):
  `pipe`, `access`, `stat64`, `open`, `getdents`, `eventfd`, `epoll_wait`,
  `epoll_create`, `creat`, `unlink`, `lstat64`, `fcntl`, `fork`, `poll`,
  `inotify_init`, `getuid`, `remap_file_pages`, `rename`, `mmap`, `dup2`,
  `compat_select:_newselect`, `mkdir`, `renameat`.
- [`bionic/libc/SECCOMP_ALLOWLIST_COMMON.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SECCOMP_ALLOWLIST_COMMON.TXT)
  -- shared with other domains. Confirmed present: `pivot_root`,
  `ioprio_get`/`_set`, `gettid`, `futex`(`_time64`), `clone`/`clone3`,
  `sigreturn`/`rt_sigreturn`, `rt_tgsigqueueinfo`, `restart_syscall`,
  `riscv_hwprobe`, `vfork`, `perf_event_open`, `tkill`, `seccomp`, `open`,
  `stat64`/`stat`, `readlink`, the `io_*` AIO family (`io_setup`,
  `io_submit`, ... -- **not `io_uring_setup`/`_enter`/`_register`**,
  confirming `runtime-failures.md`'s "`io_uring` not intercepted" is really
  "not even on the app allowlist"), `execveat`, `membarrier`,
  `userfaultfd`, the `_time64` clock/timer family, `pselect6_time64`,
  `ppoll_time64`, `recvmmsg_time64`, `rt_sigtimedwait_time64`,
  `futex_time64`, `sched_rr_get_interval_time64`.
- Assembly logic lives in
  [`libc/seccomp/seccomp_policy.cpp`](https://android.googlesource.com/platform/bionic/+/704772bda034448165d071f68b6aeca716f4220e/libc/seccomp/seccomp_policy.cpp)
  (per-arch, generated at build time from the TXT files above).
- Background: [Android Developers Blog, "Seccomp filter in Android
  O"](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html)
  -- "blocks 17 of 271 syscalls in arm64" is the **2017/O baseline**, not
  this device's policy (`main` branch's TXT files above are current AOSP,
  years of additions since O; the device here is Android 16 -- treat the
  blog's number as historical context only, not a live count).
- [source.android.com: Application Sandbox](https://source.android.com/docs/security/app-sandbox)
  -- the general sandboxing model (uid-per-app, SELinux, seccomp stacked
  together, matching `findings.md`'s own probe table).

**Gate B -- capability / kernel-config**, independent of seccomp: a
syscall can be *allowed* by the filter and still fail, because it needs a
capability the app uid never has (`CapEff = 0`, confirmed by probe) or a
kernel feature compiled out. Confirmed example from this fetch:
`pivot_root` **is** on `SECCOMP_ALLOWLIST_COMMON.TXT` -- it passes Gate A
-- but needs `CAP_SYS_ADMIN`, which `findings.md`'s probe already showed
is `0` in both the Termux app domain and the seccomp-free shell domain.
Same shape as the already-confirmed `CLONE_NEWUSER` -> `EINVAL`
(`CONFIG_USER_NS` off, kernel-wide, no app or seccomp involvement at all)
and `CLONE_NEWNS`/`mount` -> `EPERM` (`CAP_SYS_ADMIN` missing).

**Why the two gates matter for 0.5.0's scope:** own-glibc changes what
glibc does at the *libc* layer (sysconfdir, default search paths, NSS
dispatch) -- it cannot move a syscall through Gate A or Gate B, because
both are enforced by the kernel/zygote before the syscall's arguments (or
the calling library) are ever inspected. A syscall failing either gate
fails the same way no matter whose glibc issued it. Only the *shim's*
territory (libc-internal path resolution) is where own-glibc has any
leverage at all.

## Phase 2: reconcile with what's already recorded (done, 2026-09-30)

| finding | where recorded | gate | own-glibc fixes? |
|---|---|---|---|
| Debian's stock `libc6` killed at startup | `design-0.2.0.md:30` | A (unclear which syscall yet -- TODO Phase 3) | **partially** -- own-glibc *is* "Termux's glibc" in this framing, i.e. the fix is patching glibc's startup path around whatever it trips, same as Termux already does |
| `set_robust_list` SIGSYS on static binaries | `tracer-0.2.0.md` | A | no (tracer's SIGSYS emulation already answers it) |
| NSS opens (`getpwnam`, ...) via `__open_nocancel` | `shim-coverage.md`, `syscall-boundary.md` | neither -- not a kernel block, a *libc-internal symbol binding* choice | **yes** -- this is the case 0.5.0 already targets |
| `gconv`/locale modules, `ld.so.cache`, `RUNPATH` | `runtime-failures.md` B | neither -- same as NSS, loader-internal path choice | **yes**, same mechanism as NSS |
| SysV IPC (`shmget`/`semget`/`msgget`) | `runtime-failures.md` E | A or B, not yet distinguished | **TODO Phase 3** |
| `mount`, `pivot_root`, `swapon`, netlink, TUN, ports <1024, `mknod` | `runtime-failures.md` E | B (`CAP_SYS_ADMIN`/similar missing; `pivot_root` confirmed Gate-A-allowed) | no |
| `CLONE_NEWUSER` | `findings.md` probe | B, kernel-wide (`CONFIG_USER_NS` off) | no -- not even a seccomp question |
| `CLONE_NEWNS` | `findings.md` probe | B (`CAP_SYS_ADMIN`) | no |
| `io_uring*` | `runtime-failures.md` A, H | A -- absent from both allowlist TXT files | no (tracer would need to emulate it; not attempted) |

Everything in the last four rows is **Gate A or B, library-agnostic** --
own-glibc's scope should be understood as "fixes the NSS/loader-internal
row and whatever startup syscall stock `libc6` trips," not a general fix
for "things Android breaks."

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
existing probe method (`findings.md`, "Platform sandbox limits") --
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
`path-redirect.so` via `scripts/build-path-redirect.sh`). `ld-dn` only
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

**Net scope for three gates, now named with evidence each:**

| gate | enforced by | own-glibc fixes? | tracer fixes? |
|---|---|---|---|
| A: seccomp allowlist | zygote, per-syscall | no | yes, by SIGSYS emulation (already done for `set_robust_list`) |
| B: capability / kernel config | kernel, per-capability or compile-time | no | no |
| C: SELinux | policy, per-syscall/resource | no | no (same as B -- a policy/kernel decision, not a missing translation) |
| neither (libc-internal path choice) | glibc's own build config | **yes** | n/a (this is what `dn-trace`'s NSS route works around today, but own-glibc removes the need for that route entirely) |

**Closed 2026-09-30 (was "still open" above): stock Debian `libc6` doesn't
even reach a syscall question.** Tested directly: downloaded the real
`libc6_2.41-12+deb13u4_arm64.deb` from `deb.debian.org`, ran it through
the project's actual translation pipeline
(`scripts/dn-translate-deb.sh`, against a live `~/.dn` prefix) to get a
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
found the same day (`docs/findings.md`, "patchelf corrupting an `ET_EXEC`
binary's program headers") showed `dn-translate-deb.sh`'s old
`--set-rpath` step could corrupt an `ET_EXEC` binary into an identically-
shaped crash (segfault, no syscall in flight, inside loader startup).
Re-ran this section's own test material (`libc.so.6`,
`ld-linux-aarch64.so.1`, the `hello` binary -- all `ET_DYN`) through the
pipeline as it stood at the time: no corruption, clean program headers.
The bug is `ET_EXEC`-only; this section's conclusion stands.

## Termux's actual Android patch series (found 2026-09-30, `termux-pacman/glibc-packages`)

Since stock Debian glibc can't even start (above), it's worth knowing
exactly what makes Termux's build work, rather than treating it as a
black box. Fetched `gpkg/glibc/` from
[`termux-pacman/glibc-packages`](https://github.com/termux-pacman/glibc-packages)
(the primary source; `termux/glibc-packages` is a Debian-format mirror of
the same). Layout: `build.sh` (glibc 2.44, configure flags, install
steps) plus ~50 loose files -- some are unified `.patch` files against
upstream glibc source, some are whole replacement/added `.c`/`.h` files
copied in before configure.

**What `build.sh` does, in order:**
1. Deletes `clone3.S` for every arch (`clone3` disabled outright --
   independent of the seccomp-allowlist question in Phase 1; a
   toolchain/compat decision, not a permission one).
2. Copies in Termux-authored replacements: `shm{at,ctl,dt,get}.c`,
   `mprotect.c`, `syscall.c`, `setfs{u,g}id.c`, `fake_epoll_pwait2.c` --
   these implement `fakesyscall.json`'s mechanism (a declarative
   syscall-number -> C-function map compiled into
   `disabled-syscall.h`, used when a syscall Android's kernel doesn't
   support gets called -- **the same "answer instead of crash" idea as
   this doc's own Phase 5**, except done at the libc source level instead
   of a tracer intercepting after the fact).
3. Copies in `android_passwd_group.c`/`.h` + generates `android_ids.h`
   (`gen-android-ids.sh`) into `nss/` -- a **custom NSS-adjacent source
   addition**, not yet read in full; synthesizes passwd/group entries
   from Android's own uid/gid space (`android_system_user_ids.h`),
   likely how `root`/`nobody`/app uids resolve without a real
   `/etc/passwd` entry (`findings.md` already noted "glibc still
   synthesizes root/nobody/Android uids"). Needs its own read-through
   before deciding whether to keep, replace, or drop it for this
   project -- it may assume Termux's own identity model, which differs
   from this project's fake-root (0.3.0).
4. Copies in `shmem-android.c`/`.h` into `sysvipc/` -- **SysV shared
   memory reimplemented in userspace** for Android (real `shmget` is
   Gate-B/blocked, per this doc's earlier triage group 4). Doesn't change
   the "don't need SysV IPC" verdict from Phase 2b (still not a goal), but
   is useful precedent if that verdict is ever revisited: Termux already
   solved it once.
5. Runs `set-dirs.patch` (see below).
6. Bumps `version.h` to Termux's own version string.

**`set-dirs.patch`: the real NSS/path fix, confirmed as a plain,
forkable source patch.** A standard unified diff touching ~30 files
(`resolv/resolv.h`, `resolv/netdb.h`, `nss/nss_files/files-init.c`,
`nss/nss_files/files-XXX.c`, `nss/nss_compat/compat-{pwd,grp,spwd}.c`,
`nscd/nscd.h`, `misc/fstab.h`, `misc/ttyent.h`, `libio/stdio.h`, and
more) -- every hardcoded `"/etc/..."`, `"/tmp/..."`, `"/var/..."` string
replaced with a `@TERMUX_PREFIX@` or `@TERMUX_PREFIX_CLASSICAL@`
placeholder, substituted to Termux's real prefix path at build time
(ordinary `sed`, not seen yet but implied by the placeholder style).
Confirms exactly what Phase 1-3 above inferred by testing (NSS opens use
`__nss_files_fopen("/etc/passwd")` internally, unreachable by any
external interposer) -- and shows the actual, known-working fix is
this **one source patch, not a novel invention**: `nss/nss_files/
files-init.c`'s `register_file(cb, pwddb, "@TERMUX_PREFIX@/etc/passwd",
0)` is the literal line already solving case #2 for Termux's own prefix.

**What this changes about 0.5.0's cost estimate:** the patch series is
mostly plain textual path substitution across well-identified files, not
a mysterious body of Android-specific systems work. Forking it and
retargeting the placeholder to this project's own prefix path is
concretely scoped now, not an open-ended unknown -- lowers 0.5.0's
estimated cost relative to what was assumed when it was first
reprioritized above (still real work: ~30 files, a build pipeline, and
whatever `android_passwd_group.c`/`fakesyscall.json`/`shmem-android.c`
turn out to require, none of which are read in full yet).

**Open question, resolved into a decision framework (2026-09-30): fork
per-piece, judged against "a package gets exactly one view."** A Debian
package assumes one consistent system identity -- `getuid()`, `/etc/
passwd`, NSS, every API agreeing on "who am I, what does this system look
like." This project's entire architecture (shim, fake-root, the tracer's
`/etc` bind) exists to construct and hold that one illusion, over a
reality that has no real isolation at all (same kernel, same Android uid,
same seccomp filter as any other Termux process -- confirmed repeatedly
above; the "prefix" is Termux's own process wearing a costume, not a
sandboxed one). Termux's patches were written for a *different* illusion
(Termux wants to honestly reflect Android, not impersonate Debian root),
so each piece needs judging on whether adopting it reinforces this
project's single view or punctures it:

- **`set-dirs.patch` (path templating) -- fork as-is, low risk.** It only
  relocates *where* a config file is read from; it introduces no new
  identity source and directly completes work fake-root and the tracer's
  `/etc` bind already started (NSS reading the prefix's own `/etc`,
  consistent with everything else). Proven correct on-device already (the
  `hello` control test above ran through Termux's build of exactly this
  patch). Fork it, just retarget the placeholder.
- **`android_passwd_group.c` -- read in full 2026-09-30, risk revised
  down from the earlier verdict above.** It is wired in as a **fallback
  only**, appended to the tail of glibc's own `getXXbyYY.c` NSS chain via
  `#define ANDROID_SYS getpwnam_android` (`getpwnam.c.patch`, three call
  sites: `nss/getpwnam.c`, `nss/getpwnam_r.c`, `nscd/getpwnam_r.c` --
  same shape presumably for `getpwuid`/`getgrnam`/`getgrgid`, not
  individually confirmed). It only runs **after** the normal `files`
  lookup against the prefix's own `/etc/passwd`/`/etc/group` has already
  failed to find a match. Concretely: `getpwuid(0)` resolves via the
  prefix's `/etc/passwd` (`root:x:0:0:...`, always present, `base-passwd`
  provides it) -- `getpwuid_android` never even runs for that case. It
  only activates for a uid/name genuinely absent from the prefix's own
  files (e.g. a file legitimately owned by some other real Android app
  uid that fake-root never touched, since fake-root only fakes ownership
  for the process's *own* real uid/gid) -- there it synthesizes a
  readable name (`u0_a1010`) instead of leaving a bare number. **This
  does not compete with fake-root's `getuid()`/`stat`-owner answers at
  all** -- it fills a gap fake-root was never asked to cover, rather than
  contradicting it. Verdict revised: **low risk, fork candidate**, not
  "do not fork as-is." The "one view" concern from the framework above
  still applies in principle, just not to this specific file the way
  first assumed before reading it -- a lesson in itself: the framework is
  for judging what's actually read, not a substitute for reading it.
- **`fakesyscall.json` -- read in full 2026-09-30, splits into two buckets
  with very different verdicts, both now confirmed by content, not
  guesswork.**
  - **`INLINE_SYSCALL_ERROR_RETURN_VALUE(ENOSYS)` bucket -- fork
    candidate, and it **already does this project's Phase 5 idea**, at
    the libc-source level instead of the tracer.** Contains exactly
    `io_uring_setup`/`_enter`/`_register`, `set_robust_list`/
    `get_robust_list`, and the SysV IPC family (`semget`/`msgctl`/
    `msgget`/...), each compiled to return a clean `ENOSYS` instead of
    reaching the kernel and `SIGSYS`-dying. Honest, not a fake success --
    same judgment this doc's Phase 5 already reached independently. If
    0.5.0 forks this, **the tracer's Phase-5 work becomes unnecessary for
    any program linked against this glibc** (only fully static binaries,
    which bring their own libc code built against upstream Debian's
    unpatched syscall wrappers, would still need the tracer's own
    separate `SIGSYS` catch -- consistent with the loader/tracer boundary
    already established throughout this doc).
  - **The `"0"` bucket (`setuid`/`setgid`/`setreuid`/`setresuid`/
    `setfsuid`/`setfsgid`/... all unconditionally return success) --
    open question, now directly entangled with today's fake-root
    reconsideration, not a simple fork-or-don't.** This duplicates, at
    the glibc layer, exactly what this project's own fake-root shim
    already does at a different layer (`native/path-redirect.c`: "chown,
    set*id, setgroups, initgroups refused for lack of rights ->
    succeed"). Termux's version is **unconditional** -- every program
    using this glibc gets silent `set*id` success regardless of whether
    any "root illusion" is even wanted, because Android never grants real
    root either way so Termux's general-purpose build treats it as
    always-harmless. This project is currently reconsidering whether it
    even wants a root illusion at all (fake-root's tracer-side cost, and
    the identity-conflict framework above) and leaning toward "plain user
    is fine for now." Forking this bucket as-is would silently reinstate
    fake-root's exact behavior at the glibc layer even if the project
    scales fake-root back or drops it elsewhere -- the two decisions
    (own-glibc patch selection, fake-root's future) need to be made
    together, not independently, or one could quietly undo the other.

**`shmem-android.c`/`.h` -- read in full 2026-09-30, more involved than
"lowest risk" assumed above; deprioritized, not risk-graded.** Vendored
from [`termux/libandroid-shmem`](https://github.com/termux/libandroid-shmem)
into glibc directly. Real client-server IPC infrastructure, not a small
shim: creates regions via `/dev/ashmem` (`ASHMEM_SET_NAME`/`_SET_SIZE`
ioctls), then -- since ashmem regions are per-fd with no system-wide key
the way real SysV shm has -- runs a background listener thread per
process on an **abstract-namespace Unix socket**
(`sun_path[0] == '\0'`, `ANDROID_SHMEM_SOCKNAME`) that answers other
processes' requests for a given `shmid` by passing the ashmem fd over
`SCM_RIGHTS`. Two things untested for this project's actual sandbox:
whether `/dev/ashmem` opens cleanly from the Termux app uid (Termux's own
successful use of it doesn't guarantee this project's process/SELinux
context behaves the same), and whether an app-domain SELinux policy
(recall Gate C, found earlier this session) permits abstract-socket
`connect()`/`accept()` between two of this project's own processes at
all. Given Phase 2b's group 4 verdict already stands (SysV IPC: don't
need, not a current goal) -- **not worth risk-grading or reading deeper
right now**; the complexity found here is a reason to leave it deferred
alongside `io_uring`, not a reason to invest in it.

All four pieces from `gpkg/glibc/` planned for this reading pass are now
read: `set-dirs.patch`, `android_passwd_group.c`, `fakesyscall.json`,
`shmem-android.c`. Remaining unread in the same directory (`getXXbyYY.c.
patch`, `getXXbyYY_r.c.patch`, `getgrnam.c.patch`, `getgrgid.c.patch`,
`getpwuid.c.patch`, `mprotect.c`, `syscall.c`, `syslog.c`,
`disable-clone3.patch`, `disable-termios2.patch`, `dl-execstack.c.patch`,
`faccessat.c.patch`, `fchmodat.c.patch`, `fstatat64.c.patch`,
`kernel-features.h.patch`, `sem_open.c.patch`, `unistd.h.patch`, misc
`*-Makefile.patch`/`*-Versions.patch` build-glue files) -- likely smaller,
same-shape variations on patterns already seen (path templating, `getXXbyYY`
Android-fallback wiring, fakesyscall wiring); not read, no evidence yet
either way, lowest priority unless 0.5.0 work actually starts.

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
  `set-dirs.patch` alone was never claimed to be sufficient -- the
  "Termux's actual Android patch series" section above already named
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
