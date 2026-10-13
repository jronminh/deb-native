# Catching syscalls with ptrace: the primitives and how dn-monitor uses them

<!-- template: templates/docs.template.md -->

A reference for the technology under the tracer: what the kernel offers for
catching a process's syscalls, how `dn-monitor` (a pruned PRoot fork,
`src/tracer/`) composes those pieces, what a caught syscall costs, and which
alternatives exist. It describes the code as it is, with the measurements taken
for it (Samsung SM-G970W, Android 12, kernel 4.14.190, 2026-10-13). The
measurements were taken through `run-as`, whose shell has `Seccomp: 0`: Android's
own app filter was not active, so they exclude the denied-syscall path and the
cost of two stacked filters. Living doc.

## Contents

- [The primitives](#the-primitives)
- [Seccomp and ptrace together](#seccomp-and-ptrace-together)
- [How dn-monitor composes them](#how-dn-monitor-composes-them)
- [What the filter traces](#what-the-filter-traces)
- [What one stop costs](#what-one-stop-costs)
- [Measured: one trivial launch](#measured-one-trivial-launch)
- [Alternatives to ptrace](#alternatives-to-ptrace)
- [Code map](#code-map)
- [Open items](#open-items)

## Related docs

- [`../spec/monitor.md`](../spec/monitor.md) — the parts and who decides what.
- [`../spec/runtime.md`](../spec/runtime.md) — the gate page, the exec gate and
  the tiers built on this mechanism.
- [`android-platform.md`](android-platform.md) — the seccomp policy Android puts
  on the app, which this layer works around.
- [`syscall-boundary.md`](syscall-boundary.md) — what a libc interposer cannot
  reach, hence why a syscall-level catcher exists at all.

## The primitives

What the kernel gives a tracer, and which of it `dn-monitor` uses.

| primitive | what it does | used |
| --- | --- | --- |
| `PTRACE_TRACEME` / `PTRACE_SETOPTIONS` | the child declares itself traced; options choose which events stop it (`TRACEFORK`, `TRACEVFORK`, `TRACECLONE`, `TRACEEXEC`, `TRACEEXIT`, `TRACESYSGOOD`, `TRACESECCOMP`) | yes (`tracee/event.c`) |
| `PTRACE_SYSCALL` | resume until the next syscall **entry or exit**: every syscall stops twice | yes, only when an exit stop is needed |
| `PTRACE_CONT` | resume until the next event, no syscall stops | yes, the default under seccomp |
| `SECCOMP_RET_TRACE` | a seccomp filter returns it for chosen syscalls; the kernel stops the tracee with `PTRACE_EVENT_SECCOMP` **only for those**; the filter's 16-bit data rides along (`PTRACE_GETEVENTMSG`) | yes, the core of the design |
| `SECCOMP_RET_TRAP` | the kernel delivers `SIGSYS` to the process; a tracer sees it as a signal-delivery stop | not set by us; Android sets it for the syscalls it denies |
| `PTRACE_GETREGSET` / `SETREGSET` (`NT_PRSTATUS`, `NT_ARM_SYSTEM_CALL`) | read and write the registers; on arm64 `NT_ARM_SYSTEM_CALL` changes the syscall number | yes (`tracee/reg.c`) |
| `process_vm_readv` / `writev` | read and write the tracee's memory in one call, no stop per word | yes (`tracee/mem.c`); `PTRACE_POKEDATA` as fallback |
| `SECCOMP_RET_USER_NOTIF` (+ `ADDFD`) | a supervisor receives the call over a file descriptor and answers it, with no ptrace | not used, see [alternatives](#alternatives-to-ptrace) |

Seccomp resolves several filters by precedence: `KILL` > `TRAP` > `ERRNO` >
`USER_NOTIF` > `TRACE` > `ALLOW`. Android's own filter is installed by Zygote and
cannot be removed, so for a syscall it denies, its `TRAP`/`ERRNO` always beats a
`TRACE` from a filter we add. The tracer therefore cannot ask the kernel to stop
on a denied syscall; it reaches it in one of two ways: at the ptrace
syscall-entry stop, **before** seccomp runs (new kernels, see below), where it
may rewrite or void the call, or at the `SIGSYS` signal stop afterwards.

## Seccomp and ptrace together

Plain `PTRACE_SYSCALL` stops on every syscall of every process, whether or not
the tracer cares. The technique PRoot calls *ptrace acceleration* installs a
seccomp filter so only the syscalls of interest stop, and resumes with
`PTRACE_CONT` the rest of the time.

The relative order of the ptrace entry stop and the seccomp evaluation differs
between kernels. `dn-monitor` detects it at the first seccomp event and logs it
(`ptrace acceleration (seccomp mode 2, new syscall order) enabled`; the test
device is "new"). In the new order the ptrace stop precedes seccomp; the code
then skips the `PTRACE_EVENT_SECCOMP` of a syscall whose entry it already handled
(`tracee/event.c`, `seccomp_after_ptrace_enter`).

Some kernels clear the event message on every ptrace request, so the filter's
flags cannot be read back; the tracer then falls back to its own table
(`dn_monitor_sysnum_flags`) at the cost of one more request per stop.

## How dn-monitor composes them

1. **Launch.** `dn-monitor` forks with a raw `clone(SIGCHLD)`, not glibc's
   `fork()`: glibc's child calls `set_robust_list` first, which Android traps with
   `SIGSYS` before the child can `PTRACE_TRACEME` (`launch_process`).
2. **Filter.** The child installs one seccomp filter
   (`dn_monitor_install`, `monitor/monitor.c`: `PR_SET_NO_NEW_PRIVS`, then
   `PR_SET_SECCOMP`). It is inherited by every descendant and cannot be removed.
3. **One event loop.** `event_loop()` (`tracee/event.c`) waits for stops from every
   traced process and dispatches `handle_tracee_event`. All stops of all guest
   processes pass through this single loop.
4. **Resume policy.** After a stop, `restart_how` decides `PTRACE_CONT` (no exit
   stop) or `PTRACE_SYSCALL` (an exit stop is needed because the result must be
   rewritten). It is `PTRACE_CONT` unless `sysexit_necessary`.
5. **Arguments and scratch space.** At the stop the tracer reads registers, reads
   path strings with `process_vm_readv`, and, to pass a translated path, takes
   scratch memory **below the tracee's stack pointer** (`alloc_mem`, red zone
   respected), writes the string there and patches the argument register. The
   stack pointer is restored at the exit stop.
6. **Emulating a denied syscall.** `SIGSYS` is caught as a signal stop
   (`handle_seccomp_event`, `tracee/seccomp.c`); the handler either sets a result
   and swallows the signal (`set_result_after_seccomp`, e.g. `-ENOSYS`) or moves
   the instruction pointer back and restarts a substituted call
   (`restart_syscall_after_seccomp`).
7. **The gate page.** For syscalls marked `gate=yes` in `src/syscalls.tsv`, the
   filter first checks the instruction pointer: from the fixed gate page
   (`0x100000000`) the call is `ALLOW`ed, because `dn-shim` already translated it.
   Anything else falls through to the trace rules.
8. **Exec.** `execve`/`execveat` always stop at entry and exit; the exec gate reads
   the target's header and rewrites the call (`execve/`).

The filter is a program of conditional jumps evaluated **in order, for every
syscall of every process**, per architecture section:

```
arch check
for each gate=yes syscall:  if nr == S and ip in gate page -> ALLOW   (7 insns)
for each traced syscall:    if nr == S                      -> TRACE|flags (2 insns)
ALLOW
```

## What the filter traces

The TRACE list is **not** the catalog. It is `proot_sysnums` in
`monitor/monitor.c`: 98 syscalls inherited from PRoot, plus `fakeroot_sysnums`
(25, six of which overlap) while fake root is on. The catalog
(`src/syscalls.tsv`, 52 rows) supplies only the `gate=yes` exemptions (22 rows).
The filter is therefore about 140 jump tests long for a syscall that matches
nothing (`read`, `write`, `futex`, `mmap`): 22 gate blocks plus about 117 trace
rules, each tested in turn.

How a traced syscall stops depends on its flag:

- **flag `0`: one stop**, at entry, then `PTRACE_CONT`. Examples: `open`, `openat`,
  `stat` family, `mkdir*`, `unlink*`, `symlink*`, `chmod*`, `close`, `clone`,
  `clone3`, `connect`, `bind`, `prctl`, `getxattr` family.
- **flag `FILTER_SYSEXIT`: two stops**, entry and exit (the tracer needs the
  result). 35 syscalls in `proot_sysnums`, among them `brk`, `wait4`, `waitpid`,
  `ioctl` (Android builds), `uname`, `getcwd`, `readlink`, `readlinkat`, `statx`,
  `faccessat2`, `rename*`, `chdir`, `fchdir`, `socket`, `accept*`, `prlimit64`,
  `setrlimit`, `statfs*`, `mount`, `umount*`, `execve`, `execveat`.
- With fake root on, `fakeroot_sysnums` adds two-stop entries for `getuid`,
  `geteuid`, `getgid`, `getegid`, `getres[ug]id`, `getgroups`, `fstat`, `fchown`,
  `fchmod`, the `set*id` family, and makes `newfstatat`, `fstatat64`, `statx`,
  `fchownat`, `fchmodat`, `linkat` two-stop.

## What one stop costs

A stop is: the tracee blocks, the kernel wakes the tracer (`waitpid`), the tracer
fetches registers (one request), usually reads memory (`process_vm_readv`), may
write registers or memory, then resumes the tracee (one request). That is at
least two context switches and three or four system calls in the tracer, per
stop, on every syscall that stops. The cost is therefore set by:

1. **How many stops** a workload causes (the filter's traced set and each
   entry's flag).
2. **How much work** each stop does (a path translation reads and writes memory;
   the exec gate also reads the ELF header and rewrites the call).
3. **Serialization**: one event loop serves every process, so concurrent guest
   processes queue behind one tracer.

The kernels without constant-action bitmap caching for seccomp (introduced in
Linux 5.11) walk the linear filter for every syscall, traced or not. This
device's kernel is 4.14.190, so it does, and inside the app Android's own filter
is walked too.

## Measured: one trivial launch

`dn-catch` below is a 244-line prototype kept outside the tree. It keeps only
what catching syscalls needs: the launch (`clone`, `PTRACE_TRACEME`, options set
before the filter), one seccomp filter built from the same 85 traced syscalls as
`dn-monitor`, the `waitpid(-1)` loop with one restart decision per stop, register
reads and `process_vm_readv`. It does no path translation.

`dn-monitor -v 5 -- /usr/bin/true`, a dynamic glibc program started through
`dn-interp`: 23 syscall events, of which 2 are the monitor's own setup (`prctl`)
and 21 recur on every launch.

| source | stops | calls |
| --- | --- | --- |
| exec gate | 2 | `execve` to `dn-interp` (rule 3), `execve` to `ld.so` (rule 2) |
| `dn-interp` | 2 | `openat` of `dn-interp.conf`, `close` |
| `ld.so` | 11 | `brk`, `openat` of the program, `shim.so` and `libc.so.6`, `faccessat` of `ld.so.preload`, `fstat`×2, `close`×3, `prlimit64` |
| shim initialisation | 6 | `readlinkat` of `/proc`, `/sys`, `/dev`, `brk`×2, `openat` of `state/bindings` |

By kind: 11 path or exec calls, 10 that are not path calls (`close`×4, `brk`×3,
`fstat`×2, `prlimit64`).

Per launch, taken as the difference between 220 and 20 iterations of a `dash`
loop that starts `ld.so --library-path ... true` (each cell run once; the timer
resolution is 10 ms, so differences under about 1 ms are noise). `free` is the
scheduler's choice; `cpu0` is a little core and `cpu6` a gold core, with the
tracer and every tracee pinned there (`taskset`, or `dn-catch -c`):

| configuration | free | cpu0 | cpu6 |
| --- | --- | --- | --- |
| no tracer | 3.5 | 1.15 | 0.80 |
| `dn-catch`, the minimal core with the same 85-syscall filter | 8.4 | 2.45 | 2.1 |
| `dn-tiny`, the restructured prototype (below), `ld.so` started directly, rewriting the path argument of 30 syscalls | 8.6 | not run | 2.0 |
| `dn-tiny` with its exec gate, the normal path (the loop starts `/usr/bin/true`) | 8.85 | not run | 2.05 |
| `dn-monitor`, `ld.so` started directly (one exec gate, no `dn-interp`, no shim) | 14.15 | 4.95 | 3.9 |
| `dn-monitor`, the normal path (`dn-interp` and `dn-shim`) | 17.1 | not run | 4.45 |

`tools/bench/launch-cost.sh` produces these figures (modes `floor`, `direct`,
`normal`, `tiny`, `tinygate`; `-c CPU` pins). Its runs of the same configuration
differ by about 1 ms (`direct`, free: 13.3 and 14.75; `floor`, free: 3.7 and 3.0),
which is the noise to expect.

`dn-tiny` splits the same technique into four parts with a narrow interface:
`monitor.c` (the catcher: the filter, the loop, registers and memory; it knows no
syscall), `trace.c` (the applier: it reads a row and moves the bytes), `exec.c`
(the exec gate) and `policy.c` (the rows, the law for a path and the answers for
killed syscalls). It has no libc: `sys.h` and `sys.c` hold raw aarch64 syscalls,
the entry point and the few string helpers it uses, and nothing runs before
`main`. It is a 70,416-byte static executable that imports no symbol, 1,071 lines
with headers and about 750 without blanks and comments for the four parts (`sys`
adds 277 and about 150; `exec.c` is 243 and about 189). On guest paths it ran
`true`, `cat`, `uname`, `dpkg --version`, `dpkg-query -W` (reading `/var/lib/dpkg`
through translated paths), `bash` with `cd`, a write and read in `/tmp`, and,
through the exec gate, a program started by its guest path, `bash` running
external commands and a pipe, a `#!/bin/sh` script, nested `bash`, and
`dpkg -l | head`. The gate reads the head of the target, resolves a script's
interpreter, sends a glibc ELF through the loader
(`ld.so --library-path L --argv0 A program args`) and builds the new `argv` below
the tracee's stack.

A syscall the platform's filter killed (`SIGSYS` with `si_code` `SYS_SECCOMP`) is
answered at the signal stop, where the syscall was not run and the program counter
is already past the `svc`. The policy gives one of three answers:

| answer | what the tracer does | which |
| --- | --- | --- |
| a result | puts it in the result register | `ENOSYS` by default (the program's own fallback takes over); 0 for the `set*id` family and `setgroups`; `EPERM` for the privileged calls (`mount`, `clock_settime`, `sethostname`, `init_module`, ...: 17 in all, see [`device-census.md`](device-census.md)) |
| a substitute | changes the syscall number and moves the program counter back to the `svc`, so the program runs the equivalent call, which goes through our filter and has its path mapped | `faccessat2` to `faccessat`, `fchmodat2` to `fchmodat`, `openat2` to `openat` (flags and mode read from its `open_how`), `accept` to `accept4` |
| a refusal of a substitute | `ENOSYS`, when the substitute cannot honour a flag (`AT_SYMLINK_NOFOLLOW`; for `openat2`, any `RESOLVE_*`), so glibc's fallback does it | the same three |

Fake root is a different mechanism. Android does not kill `getuid` or `setresuid`;
it answers them, with the real uid and `EPERM`. So the filter catches them and the
tracer skips the syscall at entry: it writes the result register, and sets the
arm64 system call number to -1 (`NT_ARM_SYSTEM_CALL`) so the kernel does not run
it. That costs one stop where reading the result at exit costs two. Eighteen rows
are faked this way: `getuid`, `geteuid`, `getgid`, `getegid` (0), `getresuid`,
`getresgid` (three zero ids written), `getgroups` (one group, 0), the `set*id`
family and `setgroups` (0), and `fchown`, `fchownat` (0, no owner is recorded).
The set of rows with an exit stop dropped from 51 to 33.

What a call returns is rewritten at its exit stop, so the program is told what it
would be told on a real system. The row names the kind, the applier reads the
buffer, the policy gives the new text:

| result | rewritten to |
| --- | --- |
| `getcwd` | the guest path: the prefix root is removed, the prefix itself reads `/` |
| `readlinkat` | the same, for any link text that starts with the prefix root; for `/proc/self/exe` and `/proc/<pid>/exe`, the program that process runs |
| `newfstatat`, `fstat`, `statx` | the owner: the real uid and gid read 0 (so `ls -l` and `stat` show root, files the program creates included) |

`/proc/<pid>/exe` needs a record per process. The exec gate notes, at execve entry,
the real program (through the loader, the loader would be wrong) as the kernel
names it once every symlink is followed: it opens the file by `O_PATH` and reads
`/proc/self/fd/N`, so `/bin/sh` reads `/usr/bin/dash`. The record becomes valid at
the exec event (a failed `execve` does not change it) and a fork copies it. It has
no owner store (a `chown` is accepted but nothing is kept, the owner of every file
reads as root) and no `dirfd`-relative resolution.

**Inside the app** (Android's filter active, `Seccomp: 2`, `untrusted_app_27`,
cpuset `/top-app`, screen on, no `GLIBC_TUNABLES`), the freestanding build starts
and every program above ran. The glibc build of the same code is killed by `SIGSYS`
at startup three times out of three (see [`device-census.md`](device-census.md)).
The tracer answered `set_robust_list` and `rseq` once for each process it started.
A freestanding test program that calls the substituted and faked syscalls
directly got, in the app: `faccessat2` of a prefix-only path 0 and of a missing one
`ENOENT`; `fchmodat2` 0; `openat2` a descriptor that read `PRETTY_NAME="Deb`;
`faccessat2`, `fchmodat2` with `AT_SYMLINK_NOFOLLOW` and `openat2` with
`RESOLVE_NO_SYMLINKS` all `ENOSYS`; `mount` `EPERM`; the faked calls as listed
above, and `id` printing `uid=0(root) gid=0(root) groups=0(root)`. Run natively
in the app, the same program is killed on its first call. Through `run-as`
(no platform filter) the three substitutes are not exercised, since the kernel
answers `ENOSYS` itself, while the fake root results are the same. Per launch in
the app, with the loop starting `/usr/bin/true` (3 runs each):

| | free | pinned to `cpu5` |
| --- | --- | --- |
| `dn-monitor`, normal path | 23.6 to 23.9 | 4.61 to 4.66 |
| `dn-tiny`, with its exec gate | 10.1 to 12.7 | 2.17 to 2.20 |

On the Android 16 phone (SM-S7110, kernel 5.10; census in
[`device-census.md`](device-census.md)) the same freestanding tracer ran every
program above in the app, unchanged. `faccessat2`, `openat2` and `clone3` are
allowed there, so the substitutes do not fire (the SIGSYS-driven design adapts to
the device); but that exposed that `openat2` and `fchmodat2`, now run natively,
need their path mapped, so they have rows too. Per launch in the app: `dn-monitor`
normal path 5.4 to 6.6 ms free and 3.6 to 3.8 pinned to `cpu5`; `dn-tiny` 2.0 to
4.7 free (noisy) and 1.2 to 1.8 pinned (`cpu4` 1.17 to 1.25, `cpu5` 1.5 to 1.8).
One pinned run to `cpu5` returned 0 ms and was not reproduced on a repeat. Cores 6
and 7 refused a mask there too.

An earlier in-app run of `dn-monitor` read 26.2 to 26.9 ms free, so the free
figures move by a few ms with the app's scheduling state; the ratio, about 2, did
not. The affinity mask for core 6 or 7 was refused in the app (cores 0 to 5 were
accepted).

What the numbers say:

- **The translation itself is nearly free; the stops are the cost.** `dn-tiny`
  costs the same as the minimal core that only read the path (8.6 against 8.4
  free, 2.0 against 2.1 pinned), and about half of `dn-monitor`'s overhead over
  no tracer (5.2 against 10.9 ms free, 1.4 against 3.05 pinned) for the same
  85-syscall filter.
- **The exec gate adds almost nothing.** The normal path through `dn-tiny`'s gate
  costs 8.85 ms free and 2.05 pinned, the same as starting `ld.so` directly (8.6
  and 2.0), against 17.1 and 4.45 for `dn-monitor`'s normal path
  (`dn-interp` re-exec and `dn-shim`).
- **Where the work is placed matters most.** Pinning everything to one core cuts
  every row by 3 to 4 times, the no-tracer row included, so the cost is mostly
  cross-core wake-ups in a fork/exec/`wait` chain, not the stops themselves.
- **A stop is cheap once placement is fixed.** With about 23 stops per launch,
  the minimal core costs about 0.21 ms per stop free and about 0.06 ms pinned to
  `cpu6`.
- **The PRoot machinery is about half of dn-monitor's overhead.** Pinned to
  `cpu6`, `dn-monitor` adds 3.1 ms over no tracer and the minimal core adds
  1.3 ms for a comparable number of stops, so about 1.8 ms (58%) is work done at
  the stops (path translation, the exec gate, bookkeeping) rather than the stops.
- Of the cores, 0 to 3, 5 and 6 accepted an affinity mask for the app uid through
  `run-as`, and 4 and 7 returned `EINVAL`; inside the app (`/top-app`) 0 to 5 were
  accepted and 6 and 7 refused. The usable set depends on the context, so it has to
  be probed at run time.
- The repo already records a related effect on this device: a backgrounded
  process is demoted to the small cores regardless of load
  (`tools/bench/perf-run.sh`). The free numbers here therefore depend on the
  app's scheduling state when they are taken.

## Alternatives to ptrace

None of these is in the tree today.

| mechanism | idea | kernel | notes |
| --- | --- | --- | --- |
| CPU affinity | pin the tracer and its tracees to one core (`sched_setaffinity`, inherited by children) so the tracer and tracee hand off on one core | any | 3 to 4 times faster launches in the measurement above; costs parallelism, and some cores refuse the mask (cores 4 and 7 here), so it must be probed at run time |
| `SECCOMP_RET_USER_NOTIF` + `SECCOMP_IOCTL_NOTIF_ADDFD` | a supervisor answers over an fd; `openat` can be served by opening the file and injecting the descriptor | 5.0 / 5.9: **not available on the 4.14 test device** | no per-stop register fetch or resume; the tracee still blocks on the supervisor; arguments are still read from the tracee's memory; `runtime.md` lists it as tier 2 |
| in-process `SIGSYS` handler | the launcher installs a handler before the program starts, so denied syscalls are answered in the process | any | no tracer for denied syscalls; handlers do not survive `exec` unless the launcher runs again; not installed before glibc's own startup |
| a shorter or tree-shaped filter | a binary decision tree over the syscall number, or a smaller traced set | any | lowers the per-syscall walk for every process |
| fewer two-stop syscalls | move a result rewrite into the shim, or drop a handler that is not needed | any | each removed `FILTER_SYSEXIT` saves one stop per call |
| libc or shim front-run | answer in the process before the syscall (the gate page) | any | does not reach static programs, raw `svc #0` or libc-internal calls |

## Code map

| file | role |
| --- | --- |
| `src/tracer/cli/dn-trace.c` | `dn-monitor` entry, options, bindings, launches the guest |
| `src/tracer/monitor/monitor.c` | builds and installs the seccomp filter; `proot_sysnums`, `fakeroot_sysnums` |
| `src/tracer/tracee/event.c` | the event loop and `handle_tracee_event`; resume policy |
| `src/tracer/tracee/seccomp.c` | `SIGSYS` handling and emulation of denied syscalls |
| `src/tracer/syscall/{enter,exit}.c` | per-syscall entry and exit handlers |
| `src/tracer/tracee/{reg,mem}.c` | register access, memory access, scratch space on the stack |
| `src/tracer/execve/` | the exec gate |
| `src/tracer/ptrace/` | `ptrace(2)` wrappers and the emulation of tracees that ptrace each other |
| `src/syscalls.tsv`, `syscall/dn-syscalls.c` | the catalog; the gate-exempt list |

## Open items

- The tracer maps paths with PRoot's own layer (`src/tracer/path/`, 3342 lines)
  and makes no call to `dn_policy_translate_path`, `dn_policy_redirect_path` or
  `dn_policy_detranslate_path`; it calls `dn-policy` only for fake root and
  hardlinks. The shim and the tracer agree on paths only because the tracer is
  given `dn-policy`'s binding rows at start (`dn-trace.c`). The tracer has 97
  entry handlers and 47 exit handlers (`syscall/enter.c`, `syscall/exit.c`, about
  3750 lines) and about 19400 lines in all, without talloc.
- `monitor.md` says the filter is built from the catalog; in the code the catalog
  supplies only the gate-exempt list and the trace list is `proot_sysnums`
  (a `TODO` in `dn_monitor_install` says it should be filtered "only if path
  translation is required").
- `PTRACE_O_EXITKILL` is not set (`monitor.md`, "Where the model leaks").
- The cost of the linear filter per syscall, and whether this kernel caches
  constant-action syscalls, are not measured.
- Which of the `close`, `brk` and `prlimit64` handlers are needed for this
  runtime (they come from PRoot) is not checked.
