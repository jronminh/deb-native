# The contract: what the prefix promises a program

Status: **current** (2026-10-18).  T1 is in and frozen; T2 is an open question.
The frame for deciding which kernel facilities the prefix provides, at what
fidelity and at what cost; some rows are implemented, and the rest stay best
effort or refused until a named workload opens them.  How the mapping works is
[`mapped-kernel.md`](mapped-kernel.md) and [`../architecture.md`](../architecture.md);
the per-syscall mechanics are
[`../reference/syscall-classes.md`](../reference/syscall-classes.md).

The mapping says *how* a call is answered.  This says *what a program may rely
on*: which parts of the Linux interface are a promise, which are best effort,
and which are refused.  Without it, "add a feature" is a list of anecdotes; with
it, a feature has a class, a strategy and a test.

## The universe and the actual

The **universe** is the Linux arm64 ABI a Debian userland is built against:
the syscall set, the pseudo-filesystems (`/proc`, `/sys`, `/dev`), netlink,
`ioctl`, sockets, signals, namespaces and cgroups.  The arm64 table is one
generic list and only grows; Debian 13 (trixie) ships the 6.12 headers, **328**
real syscalls ([`syscalls-by-kernel.md`](../reference/syscalls-by-kernel.md)).

The **actual** is the phone's kernel inside the app domain.  On the S10e
(4.14) 201 numbers are allowed, 256 killed, 32 of the killed are real denials
(the kernel implements them) and 224 are absent; on the S23 FE (5.10) the
numbers are 217 / 240 / 42 / 198
([`device-census.md`](../reference/device-census.md)).  The app domain also
hides `/proc` files, keeps the SELinux label, and lacks some kernel config and
devices.

So there are **three deltas**, and a call that fails with `ENOSYS` can mean any
of them.  The strategy differs:

| delta | what it is | example | what can be done |
| --- | --- | --- | --- |
| **version** | the host kernel is older than the reference | `clone3` (5.3), `faccessat2` (5.8), `openat2` (5.6) on 4.14 | substitute the older call when one exists; else `ENOSYS` |
| **config** | the kernel was built without the facility | SysV IPC (`CONFIG_SYSVIPC` off: no `/proc/sysvipc`), POSIX mqueues | only a broker in user space can provide it |
| **policy** | the app sandbox's kernel side (SELinux, capabilities) denies a call the kernel has | `unshare`, raw sockets, parts of `/sys` | answer or refuse; enforcement cannot be restored |

The config delta was checked on the S10e on 2026-10-18: `/proc/sysvipc`,
`/proc/sys/kernel/shmmax`, `/proc/sys/fs/mqueue` and `/dev/shm` are absent, and
`shmget`/`semget`/`msgget`/`mq_open` answer `ENOSYS` in the prefix.

### A second gate: seccomp

The three deltas describe the **kernel** gate.  A call also passes the app's
**seccomp** filter, and the two gates are independent: bionic's policy is
`SYSCALLS.TXT - blocklist + allowlist`, and a number outside it is killed with
`SIGSYS` before the kernel sees it.  The tracer runs in the same app, under the
same filter, so it **cannot re-issue a killed call**; it can answer it,
substitute an allowed call, or implement the effect in user space.

| seccomp | kernel | example | strategy |
| --- | --- | --- | --- |
| allowed | native | `read`, `mmap`, `unshare`* | pass (the kernel answers) |
| allowed | version/config | `clone3` on 4.14, `fchmodat2` | substitute / `ENOSYS` |
| killed | native | `setuid`, `accept`, `faccessat2` | fake or substitute an allowed call |
| killed | version/config | SysV, `mq_*`, `io_uring`, keyrings | emulate in user space (broker) or refuse |

\* `unshare`/`setns` are in `SYSCALLS.TXT` (seccomp allows them); what refuses
them is SELinux/capabilities, not seccomp.

The source check and the per-call findings are in
[`../reference/seccomp-gates.md`](../reference/seccomp-gates.md).

`uname` and `/proc/version` are **not** faked: the phone is a real Linux on real
arm64, and the contract says so ([`phase2.md`](phase2.md)).  The reference
kernel is the *userland's expectation*, not a version the runtime claims; a
program sees the host's real version, as it would on an older Debian kernel.

## The interfaces

Five kinds of surface, each with its own way of being answered:

1. **syscall** -- the arm64 table; the rows in `gen_rows.py` say what is caught
   and the law (`policy.c`) says what a path or an id becomes.
2. **pseudo-file** -- `/proc`, `/sys`, `/dev`, `/run`; a read is a read, so a
   file Android hides is generated at open (`synth_*`).
3. **netlink** -- route, audit, kobject; some families are killed, some answers
   are lies the kernel will not tell.
4. **ioctl / device** -- the host's nodes under `/dev`; `ioctl` is never stopped,
   so a node is either exposed through the bind table or it is not.
5. **kernel-maintained names** -- the mount view, `getcwd`, `readlink`, the
   `/proc/<pid>/{cwd,exe,fd}` links, `AF_UNIX` paths; the law maps them out as
   well as in ([`coverage.md`](coverage.md)).

## Levels and fidelity

Every facility gets one level:

- **Promised** -- the runtime guarantees it and a test proves it.  A regression
  is a bug, not a device quirk.
- **Best-effort** -- provided where the host or device allows, with a clear
  error otherwise.  It can differ between phones; it is not in the promise.
- **Refused** -- a truthful `EPERM`/`ENOSYS` with a reason.  Never an invented
  success.

and one fidelity:

| fidelity | meaning | example |
| --- | --- | --- |
| **exact** | the host does the work; the answer is the kernel's | `read`, `mmap`, `inet` sockets |
| **mapped** | the answer is the same after a change of coordinates | a path, an `AF_UNIX` `sun_path` |
| **view** | the program sees a consistent world, but the kernel's enforcement is the app's | identity, capabilities, the mount view |
| **refused** | the call says no, and says why | `mount`, `init_module`, `bpf` |

A **view** is a promise about what a program *reads*, not about what the kernel
*enforces*.  The difference is stated wherever it matters
([`identity.md`](identity.md); "Tree-root is not real root").

## Choosing: the rules

For a facility the userland asks for, walk these in order; stop at the first
that applies:

1. **Pass** if the host already does it with the same semantics (no transform,
   no stop).
2. **Map** if only a coordinate differs (path, id, socket name), by the one law.
3. **Answer** if the program only *reads* a fact the app domain hides: the real
   value when the tracer can read it, the Debian identity when the answer is the
   prefix's, a clear placeholder when neither exists.
4. **Broker** if processes need shared state the kernel does not offer
   (`CONFIG_*` off, or the call killed) but no enforcement is implied: a segment
   is a `memfd`, the key/id a record, the fd passes over a unix socket.
5. **Pass through** if it is a device the host has and the bind table exposes;
   the ioctl is the kernel's (and the node's risk is the device's).
6. **Refuse** if the facility is kernel-enforced isolation/scheduling
   (namespaces, cgroups, modules, eBPF, kexec) or if a fake would be a lie
   about authority.  The refusal names the reason.

The ranking, when there is a choice:
**value = (program demand x fidelity gained) / (cost + lie risk + fast-path
tax)**.  A feature that is demanded by a real workload and changes a refusal
into an answer ranks highest; a feature that taxes a hot path ranks lowest.

Two invariants hold for every row:

- **One law, two engines.**  An answer lives in one model that the tracer and
  the shim both read; no model may exist in only one of them.
- **No new lie about authority.**  A view may be approximate; it may not claim
  isolation, privilege or a kernel number that is not real.

## The matrix

Representative rows; "host today" is the app domain on the two phones of the
census, and "proof" is where the row is exercised.  The full per-call map stays
in [`syscall-classes.md`](../reference/syscall-classes.md).

### Process, memory, time

| facility | interface | host today | strategy | level | proof |
| --- | --- | --- | --- | --- | --- |
| `fork`/`clone`/`execve`/`wait`/signals | syscall | native | pass (exec: gate) | promised | bench, `mt`, `fr.sh` |
| `clone3` | syscall | version (4.14); native on newer | `ENOSYS` (glibc falls back) | promised | `syscalls.c` |
| `faccessat2`/`fchmodat2`/`openat2` (no resolve flags) | syscall | version/policy | substitute the older call (`openat2` with `RESOLVE_*` -> `ENOSYS`) | promised | `syscalls.c` |
| `mmap`/`mremap`/`madvise`/`memfd` | syscall | native | pass | promised | toolchains |
| `futex`/`epoll`/`eventfd`/`timerfd`/`signalfd` | syscall | native | pass | promised | `mt`, Lua |
| `io_uring` | syscall | seccomp kill; version on 4.14, the kernel has it on 5.10 | refuse `ENOSYS` | refused | gates |
| `clock_gettime`/timers | syscall | native | pass | promised | all |
| `clock_settime`/`settimeofday`/`adjtimex` | syscall | policy (`EPERM` as non-root) | refuse `EPERM` | refused | census |
| `uname`/`/proc/version`/`cpuinfo` | syscall / pseudo-file | native, real kernel | pass (no fiction) | promised | fingerprint |

### Files and names

| facility | interface | host today | strategy | level | proof |
| --- | --- | --- | --- | --- | --- |
| path-bearing calls | syscall | native + law | map | promised | `coverage` |
| `stat`/`statx`, owner, `chown` | syscall | native + owner store | answer (view) | promised | `owner.sh`, `fr.sh` |
| `access`/`faccessat` | syscall | app's ids | answer from the virtual ids | promised | `access.sh` |
| `getdents`, `readlink`, `getcwd`, `/proc/*/{cwd,exe,fd}` | syscall / pseudo-file | kernel names the host | map out | promised | `coverage` |
| mounts, `mountinfo`, `/etc/mtab`, `maps`, `smaps` | pseudo-file | hidden/host | answer from the table | promised | `coverage` |
| `[f]getxattr`, `[f]listxattr` | syscall | SELinux label present | answer absent | promised | `coverage` |
| `mkdirat`/`mknodat`/`openat`+`O_CREAT` | syscall | native + marking | map + mark | promised | `owner.sh` |
| hard links (`linkat`) | syscall | policy (SELinux) | copy + `st_nlink` view | promised (approximate) | `owner.sh` |
| `mount`/`umount2`/`chroot` (killed) and `pivot_root` (runs) | syscall | policy | refuse `EPERM` (the first three could be law-mapped later; `pivot_root` is the kernel's own `EPERM`) | refused | census |
| `/proc/sys/*` | pseudo-file | partly hidden | answer the needed defaults, else the kernel | best-effort | TODO |

### IPC, sockets, network

| facility | interface | host today | strategy | level | proof |
| --- | --- | --- | --- | --- | --- |
| pipes, `AF_UNIX` (path mapped), `AF_INET` | syscall | native | pass / map the `sun_path` | promised | `coverage`, tmux |
| socket options, `SO_PEERCRED` | syscall | kernel's app ids | pass (the peer's ids are a residue) | best-effort | docs |
| SysV `shm*`/`sem*`/`msg*` | syscall | double-blocked: seccomp kill + `CONFIG_SYSVIPC` off | broker (open question; `memfd` + unix socket) | refused today | census, gates |
| POSIX `mq_*` | syscall | double-blocked: seccomp kill + config off | broker or refuse | refused today | census, gates |
| POSIX shm (`shm_open`) | libc over `/dev/shm` | no `/dev/shm` row in the tree | provide a tmpfs (with a shm broker) | refused today | probe |
| netlink `route`/`kobject` | netlink | partly allowed | pass / refuse per family | best-effort | census |
| netlink `audit` | netlink | policy | `EPROTONOSUPPORT` (libaudit tolerates) | promised | `xm`, sudo |
| raw sockets, netfilter/`iptables` | syscall | no `CAP_NET_ADMIN` | refuse `EPERM` | refused | docs |
| `ptrace` | syscall | same app only (SELinux) | pass; cross-app is the host's no | best-effort | runtime itself |

### Identity, security, kernel

| facility | interface | host today | strategy | level | proof |
| --- | --- | --- | --- | --- | --- |
| `get*id`, `getres*id`, `set*id`, `getgroups`, `setgroups` | syscall | policy + one real uid | answer from per-process state | promised | `fr.sh`, `su` |
| setuid-bit exec | syscall (exec gate) | native exec + gate | answer euid | promised | `su`, sudo |
| `/proc/<pid>/status` (`Uid`, `Gid`, `Cap*`, `NoNewPrivs`, `Seccomp`, `TracerPid`) | pseudo-file | hidden/leaky | answer | promised | coverage |
| SELinux label | xattr | present | answer absent | promised | coverage |
| capabilities (`status` view) | pseudo-file | app's | view; `capget`/`PR_CAPBSET_READ` refused | view / refused | identity |
| `prctl(PR_GET_NO_NEW_PRIVS)`, `PR_SET_DUMPABLE` | syscall | policy | answer (0) / ignore | promised | sudo |
| namespaces (`unshare`, `setns`, clone flags) | syscall | seccomp allows; SELinux/capabilities deny | refuse `EPERM`/`ENOSYS` | refused | gates |
| cgroups | pseudo-file | `/sys/fs/cgroup` hidden (`EACCES`); `/proc/<pid>/cgroup` answered as `0::/` | answer the process file; a cgroup tree is refused | view / refused | phase2, probe |
| `init_module`/`delete_module`/`kexec`/`reboot` | syscall | policy | refuse `EPERM` | refused | census |
| `bpf` | syscall | killed on Android 12; allowed on 16, the kernel says `EPERM` | refuse `EPERM` | refused | gates |
| keyrings (`add_key`, `request_key`, `keyctl`) | syscall | seccomp kill + kernel `ENOSYS` | `ENOSYS` | refused | gates |
| `perf_event_open` | syscall | allowed; the host's perf/SELinux policy says `EACCES` | pass (the host's answer) | best-effort | census, probe |
| `/dev` nodes, `ioctl` | device | host's nodes via binds | pass through (the node's truth) | best-effort | GPU TODO |

## Tiers

The order of work, by the userland's demand, not by the syscall table:

- **T1 -- base and dev**: the rows above marked *promised*; a Debian trixie
  prefix with `apt`, `dpkg`, gcc, git, python, sudo/su/passwd.  **This is the
  milestone; it is in.**  The feature set is frozen here: no new promised row
  without a named workload and a test.
- **T2 -- service**: a broker for SysV shm/sem/msg (PostgreSQL, X/Steam),
  POSIX shm, pty/utmp, a netlink family.  **Open question, not planned**: the
  survey puts it in the expensive quadrant (seccomp-killed + config off), and a
  broker is a new stateful subsystem, not a row.  A named workload and a
  time-boxed spike are the way in, not a general commitment.
- **T3 -- desktop**: device passthrough (GPU, input, audio), Wayland/X over a
  Windows/Host client.  **Later, only with a target.**
- **T4 -- isolation**: namespaces/cgroups enforcement, containers, modules.
  **Out of scope**; that is the machine boundary (a VM), not a prefix.

### Hosts

The same runtime has three hosts, and the promise is the same
([`hosts.md`](hosts.md)):

- **Termux** and **custom app**: Android's seccomp filter plus the runtime's
  rows; the app runtime (the default build) carries the app-domain answers
  (`policy_deny`, SIGSYS substitutes); the prefix is in the app's data.  They
  differ in the app, session, UI and storage, not in the promise.
- **adb/shell**: no seccomp gate; a runtime built with `--host adb` has no
  app-domain answers at all; the prefix is in `/data/local/tmp` (uid 2000),
  `/sdcard` is bound read-only, and a PC drives it over `adb shell`.  The
  kernel gate (version, config, SELinux/capabilities) is unchanged.  See
  [`adb-host.md`](adb-host.md).

## Keeping it true

- The **census** is the host delta (which numbers are allowed/killed/missing).
- **`syscalls-by-kernel.md`** is the version delta.
- **`syscall-classes.md`** is the per-call transform/placement/validation.
- **This page** is the promise: level, fidelity, strategy, proof.
- Every *promised* row has a test (`tests/bench`, `scripts/coverage.sh`,
  `scripts/fingerprint.sh`); a row without one is *best-effort* by definition.
- When a row changes, all four move together.  A feature request starts here:
  find the row, argue the level, add the test.

## Open questions

1. **The reference version**: keep the host's real `uname` (current) while the
   *expectation* is the 6.12 headers, or expose a Debian `version=` string
   somewhere else (`boot/version` already names the suite).
2. **Broker lifetime**: a segment must outlive the process that created it, but
   a prefix is one boot.  Who owns the records (the tracer, a small daemon,
   `/run`)?  Garbage collection after a session crash?
3. **Substitute or refuse** for a version-missing call: `clone3`->`clone` is
   safe for glibc, but a direct-syscall program may prefer `ENOSYS`; keep the
   rule in one place.
4. **Devices**: which `/dev` rows are safe to expose, given `ioctl` is never
   stopped and a node is the host's.
5. **Fast-path budget**: how many stops a promised facility may add before it
   must move into the shim/loader fast path.
6. **Views of enforcement**: for cgroups/namespaces, is a view honest enough to
   offer, or is the refusal the only truthful answer?  (Current bias: refuse.)

## References

In the repository: [`mapped-kernel.md`](mapped-kernel.md),
[`plan.md`](plan.md), [`phase2.md`](phase2.md), [`coverage.md`](coverage.md),
[`identity.md`](identity.md),
[`../reference/syscall-classes.md`](../reference/syscall-classes.md),
[`../reference/device-census.md`](../reference/device-census.md),
[`../reference/seccomp-gates.md`](../reference/seccomp-gates.md),
[`../reference/syscalls-by-kernel.md`](../reference/syscalls-by-kernel.md).

Outside:

- `syscalls(2)`, the per-call list and kernel versions:
  <https://man7.org/linux/man-pages/man2/syscalls.2.html>
- the arm64 table (one generic list):
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/include/uapi/asm/unistd.h>
  and `include/uapi/asm-generic/unistd.h`
- namespaces and cgroups, the facilities the prefix refuses:
  <https://man7.org/linux/man-pages/man7/namespaces.7.html>,
  <https://man7.org/linux/man-pages/man7/cgroups.7.html>
- the bionic seccomp sources: `libc/SYSCALLS.TXT`,
  `SECCOMP_BLOCKLIST_APP.TXT`, `SECCOMP_ALLOWLIST_COMMON.TXT` (the
  `android12-release` branch for the S10e, `main` for Android 16):
  <https://android.googlesource.com/platform/bionic/>
- the GKI kernel config fragments (`android-base.config`, which has
  `# CONFIG_SYSVIPC is not set`): <https://android.googlesource.com/kernel/common/>
- the kernel's own user-space API docs: <https://docs.kernel.org/>
