# The prefix owns the syscall entry

Status: **proposal, for discussion** (2026-10-23). Not current truth. This
records a direction: the prefix takes every syscall first, in the process that
made it, and decides there whether to pass it, fix it, or answer it itself; the
tracer drops back to a net. Nothing here is measured on the phones yet except
what is cited from the experiments.

## Why: the tracer's cost is the wake, not the work

From `docs/reference/experiments.md`:

- E1: one stop costs ~150 us with the tracer asleep and ~17-19 us with it
  polling (`-s 500`). The work (the law, the path map) is the same in both, so
  ~130 us of the sleeping stop is waking the tracer's core.
- E11: the only lever that moved the number is keeping the tracer's core awake;
  pinning, `nice` and busy cores all made it worse.

So the tracer's own logic is not the bottleneck. Even fully awake, a stop keeps
a floor: the tracee stops, the tracer runs a few syscalls of its own (registers,
the path, registers back, resume), and the tracee runs again. Outside numbers
agree: rr counts four context switches per traced call and found ptrace
switching the main cost for low-parallelism workloads ([rr](#rr)); Nexpoline
measures `getpid` at ~33,000 cycles under ptrace vs ~215 native and ~5,200 with
an in-process `SIGSYS` handler (x86, [Nexpoline](#nexpoline)). gVisor dropped its
ptrace platform for the same reason ([gVisor platforms](#gvisor-platforms)).

Total cost is stops x cost per stop. Polling already took the largest cut off
the cost per stop; what is left there is small and has a floor. The rest has to
come from not stopping.

A measurement still owed: `stopbench` under `-s 500` with the tracer resuming at
once vs mapping as usual. The difference is the tracer's work; if it is ~1 us
the claim above is measured, not argued.

## The four ways to catch a syscall

Without root, the kernel offers four:

| mechanism | who answers | wakes another process | S10e (4.14) | S23 FE (5.10) |
| --- | --- | --- | --- | --- |
| ptrace (`SECCOMP_RET_TRACE`) | the tracer | yes | yes | yes |
| `SECCOMP_RET_USER_NOTIF` | a supervisor | yes | no | yes |
| Syscall User Dispatch | the process | no | no | no |
| `SECCOMP_RET_TRAP` -> `SIGSYS` | the process | **no** | yes | yes |

- `USER_NOTIF` does not escape the wake. Its sync mode (same-CPU wakeup, ~12 ->
  ~3 us per round trip) needs Linux 6.6 ([sync wake](#unotify-sync)); neither
  phone has it. It also cannot rewrite a call: the supervisor returns a value or
  lets the call run (`CONTINUE`, a TOCTOU risk), so path mapping would become
  "do it on the target's behalf" with `SECCOMP_IOCTL_NOTIF_ADDFD` (5.9+)
  ([seccomp_unotify(2)](#unotify-man)). A model change for no speed gain.
- SUD is out of reach on both phones.
- `SIGSYS` is the only one that catches everything and wakes no one.

## The model: a middleman in the process

The `SIGSYS` path is the tracer's flow without ptrace:

| step | tracer | `SIGSYS` handler |
| --- | --- | --- |
| catch | `RET_TRACE`, the tracee stops | `RET_TRAP`, the kernel signals the same process |
| who runs | another process, woken | the calling thread |
| number + arguments | `PTRACE_GETREGSET` | the `ucontext` the kernel hands the handler |
| the path | `process_vm_readv` / PEEK | read directly, same memory |
| decide | the law | the same law |
| answer | set registers, resume | set `x0` in the `ucontext`, `sigreturn` |

No instruction is changed. One difference matters: a trapped call cannot be
"continued". The original call is dropped; the handler issues its own (with the
"already resolved" marker the filter lets through, the cookie or E13's gate-page
IP) and returns its result. The program's call never reaches the kernel as is.

Patching an `svc` site is a different thing: it is the only one that changes
code, and in this model it is a shortcut to the same middleman, skipping the
signal. A wrong patch then falls back to `SIGSYS`, not to a broken program.

| entry | changes code | catches all | cost |
| --- | --- | --- | --- |
| patched `svc` | yes | no | 0 stops |
| symbol interposition (LD_PRELOAD) | no | no, dynamic glibc only | 0 stops |
| in-process `SIGSYS` | no | yes | a few us (unmeasured on ARM) |
| tracer | no | yes | ~17 us |

The tracer stays as the last net, for what the process cannot see: a process
born outside `dn-exec`, a handler not yet installed, a crash.

This is not a library OS in content. The default is still passthrough;
emulation is the exception (`runtime-revision.md`). What moves is *where* the
decision is made, not *how much* is made up. In shape it is close to one (a
library OS also takes syscalls in-process first); in content it is restoring
what Android removed.

## Owned syscalls as a fast path

A syscall the prefix answers itself in-process never reaches the tracer.
`dn-exec` is the precedent: it adds no feature and cut spawn stops by 42%.
Under this model a row of the table is either *fix* (map the path, re-issue the
real call) or *own* (the handler does the whole job and uses the kernel only for
steps, as `dn-exec` uses `mmap`). Same entry, same table.

Where the remaining stops are (`docs/performance.md`, `runtime-revision.md`):

| source | stops | owned in-process |
| --- | ---: | --- |
| `getdents64` (the `/proc` filter row) | ~2871 in `find` (one per directory) | filter only when the fd is a `/proc` directory; the rest passes |
| fork/exec events | 258 (Lua) | if the exec gate runs in the process, the tracer no longer needs the fork events |
| `execve` | 195 (Lua) | the same move: the gate in the caller, then exec `dn-exec` directly |
| `SIGSYS` from Android's filter | 263 (Lua) | answered by the in-process handler |
| libc's cancellable `openat` | 324 (Lua) | the build-time-offset PoC |
| `unlinkat` | 79 (Lua) | a lane row |
| identity | 20 (Lua) | answered in-process entirely |

`getdents64` is the cheapest large win (a path-only call, no new mechanism).
Moving the exec gate is the structural one: `dn-exec` owns the load, the gate
still owns the decision (shebang, permission, identity) in the tracer. With both
in the process, ptrace leaves the process lifecycle.

Adding features (SysV IPC, mqueue) can use the same owned rows; that is separate
work with its own criterion: own a call only when the kernel cannot give the
right answer and programs need it. Two rules either way: every reader agrees (an
owned SysV shm shows in `/proc/sysvipc/shm`, owned ids in `/proc/self/status`),
and programs only ever see the standard Linux ABI; a private syscall number is
for runtime components talking to each other, never for the tree.

## E12, revisited

E12 failed because a seccomp filter survives `exec` and a signal handler does
not: the loader's `openat` trapped before the shim's constructor installed the
handler. `dn-exec` now owns birth: it maps the loader (or a static program) and
jumps, so it can install the handler before the first instruction of the new
image. That closes E12's window, for static programs too.

## What is hard

1. **The program blocks or replaces `SIGSYS`.** A blocked or ignored `SIGSYS`
   makes a seccomp trap kill the process. `rt_sigaction` and `rt_sigprocmask`
   must keep `SIGSYS` live; `rt_sigprocmask` is frequent, so it needs the
   patched path, not a trap per call.
2. **Telling our trap from Android's.** The filter's `SECCOMP_RET_DATA` arrives
   as `si_errno`; ours carries a known value.
3. **Signal context.** The handler runs inside the program's code: it keeps
   `errno`, survives nested signals, runs on its own stack per thread
   (`sigaltstack`), and copes with `vfork`. Programs that use `SIGSYS`
   themselves (Chrome's sandbox) are rare and need a fallback.
4. **Not a boundary.** The handler shares memory with the program, which can
   break it. That fits the scope (compatibility; a VM for isolation).
5. **Every entry covered.** exec through `dn-exec`, direct and foreign-libc
   calls through `SIGSYS`, signals kept live, threads from raw `clone` given a
   stack; a process from outside the prefix is the one case left to ptrace.

## Prior art

- **Chromium**: the Linux sandbox docs list what a `SIGSYS` handler can do with a
  blocked call: log and return an errno, emulate it, or broker it out over IPC
  ([Chromium](#chromium)).
- **Firefox**: `SandboxBrokerClient` provides open, access, stat, lstat, chmod,
  link, readlink, mkdir, rmdir, rename, unlink and connect, meant to be called
  from a seccomp-bpf `SIGSYS` handler to replace those calls transparently, from
  any thread and in signal handlers ([Firefox](#firefox)). Nearly the prefix's
  path set, in production at large scale.
- **gVisor Systrap**: ptrace only to set up executor threads; a restrictive
  filter, a signal handler and shared memory per thread; the `SIGSYS` handler
  posts the call to the Sentry; a recognized `mov; syscall` site is patched to a
  jump so the next call skips seccomp and the signal (x86 only) ([Systrap](#systrap)).
  The three-level shape proposed here.
- **Syscall User Dispatch**: motivated by Wine emulating syscalls for part of a
  process in-process, delivered as `SIGSYS`; explicitly not a sandbox; reset by
  fork and exec ([SUD](#sud)) -- E12's window, in the kernel's own words.
- **LXD**: emulates only `mknod` (allowed devices) and `mount` through the
  seccomp notifier, with filters kept narrow; each call is a supervisor round
  trip ([LXD](#lxd)). "A few owned calls", but out of process.

What they share: the handler sits in the program, the work sits in **another
process** (broker, Sentry, LXD), because their goal is security and the trusted
part cannot share memory with untrusted code. So they still pay a cross-process
handoff, only cheaper than ptrace. deb-native's goal is compatibility, so the
work can stay in the handler: SUD/Wine-style in-place emulation plus
Systrap-style site patching. No arm64 project doing exactly that combination
for compatibility was found (2026-10-23).

## Next: E14

Measure the in-process `SIGSYS` path on the S10e before building on it: a
freestanding loop of `mkdirat` under a `RET_TRAP` filter, a handler that maps
the path and re-issues the call with the marker, against `dn-tiny -s 500`'s
~17 us. Below ~5 us per call makes the direction worth a POC; the only cost
figure today is Nexpoline's x86 table.

## Open questions

1. Handler installed by `dn-exec` at birth, or by the shim with `dn-exec`
   covering only the loader window?
2. Which marker for re-issued calls: the cookie (`args[5]`) or E13's gate-page
   IP, which may fit a handler better than it fit the shim?
3. Does the tracer still need `PTRACE_O_TRACEFORK` once the gate is
   in-process, or only a watch for processes born outside `dn-exec`?
4. BTI/PAC: the handler and any patched stub on a guarded page need landing
   pads (`inline-hooking.md`); check `/proc/cpuinfo` on the S23 FE, whose cores
   (Cortex-X2/A710/A510) implement both.

## References

- <a id="rr"></a>O'Callahan et al., *Engineering Record And Replay For
  Deployability* (rr), arXiv 1705.05937 -- <https://arxiv.org/pdf/1705.05937>
- <a id="nexpoline"></a>*System call interposition without compromise*
  (Nexpoline), DSN 2024, arXiv 2406.07429, Table III --
  <https://arxiv.org/html/2406.07429>
- <a id="gvisor-platforms"></a>gVisor, *Platform Guide* --
  <https://gvisor.dev/docs/architecture_guide/platforms>
- <a id="systrap"></a>gVisor, *Releasing Systrap* (2023-04-28) --
  <https://gvisor.dev/blog/2023/04/28/systrap-release/>
- <a id="unotify-sync"></a>LKML, *seccomp: add the synchronous mode for
  seccomp_unotify* -- <https://lkml.iu.edu/hypermail/linux/kernel/2301.1/03410.html>;
  the perf benchmark -- <https://lkml.iu.edu/hypermail/linux/kernel/2303.1/00075.html>
- <a id="unotify-man"></a>`seccomp_unotify(2)` --
  <https://man7.org/linux/man-pages/man2/seccomp_unotify.2.html>
- <a id="chromium"></a>Chromium, *Linux sandboxing* --
  <https://chromium.googlesource.com/chromium/src/+/main/docs/linux/sandboxing.md>
- <a id="firefox"></a>Firefox, `security/sandbox/linux/SandboxBrokerClient.h` --
  <https://searchfox.org/firefox-main/source/security/sandbox/linux/SandboxBrokerClient.h>
- <a id="sud"></a>Linux, *Syscall User Dispatch* --
  <https://docs.kernel.org/admin-guide/syscall-user-dispatch.html>
- <a id="lxd"></a>Christian Brauner, *The Seccomp Notifier -- New Frontiers in
  Unprivileged Container Development* --
  <https://people.kernel.org/brauner/the-seccomp-notifier-new-frontiers-in-unprivileged-container-development>
