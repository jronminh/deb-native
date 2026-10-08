# Backend emulation: faking what the kernel refuses

<!-- template: templates/docs.template.md -->

Android's kernel and SELinux refuse some operations outright, and no privilege
obtainable from the app uid unlocks them (see
[`android-platform.md`](../reference/android-platform.md)). Rather than fight
the platform, the runtime reimplements the refused subsystem in userspace and
answers every interface that reads it from one shared model, so a program sees
a coherent result. This doc is the design for that layer: what is faked, by
what, and what must agree. Status: design; only the pieces under
[Already emulated](#already-emulated) exist today.

## Contents

- [The principle](#the-principle)
- [Two kinds of hard limit](#two-kinds-of-hard-limit)
- [Already emulated](#already-emulated)
- [Virtual filesystem](#virtual-filesystem)
- [Synthetic /proc and /sys](#synthetic-proc-and-sys)
- [Network](#network)
- [Identity and capabilities](#identity-and-capabilities)
- [Namespaces and pid](#namespaces-and-pid)
- [Consistency: the real work](#consistency-the-real-work)
- [Refuse, don't fake](#refuse-dont-fake)
- [Priorities](#priorities)

## Related docs

- [`overlay.md`](overlay.md) — the runtime (`dn-policy`, `dn-glibc`,
  `dn-trace`) this layer is part of.
- [`native.md`](native.md) — the execution model this layer serves (near-full
  behaviour, near-native speed).
- [`../reference/android-platform.md`](../reference/android-platform.md) — the
  three enforcement gates this layer answers around.
- [`../reference/known-issues.md`](../reference/known-issues.md) — "Gaps
  toward a complete Debian", the open list.
- [`../../TODO.md`](../../TODO.md) — the scheduled work.
- [`../../src/dn-policy/`](../../src/dn-policy), [`../../src/tracer/`](../../src/tracer)
  — where the mechanisms live.

## The principle

The rule is *answer in place of the kernel*, never *make the kernel do it*.
A refused call (`mount`, `unshare`, a raw socket, a low-port bind, a capability
change) is intercepted, recorded in a model that lives in dn-policy, and
answered with the result a permitted system would give. The program never
learns the operation did not reach the kernel.

Two properties make it hold:

- **One model, two enforcement points** ([`overlay.md`](overlay.md) principle
  3): dn-glibc answers the same question as dn-trace, from the same state, so
  the fast and traced paths cannot disagree.
- **Every reader is covered.** A model is only as good as its
  least-covered reader: if `bind()` is translated but `/proc/net/tcp` still
  shows the raw port, a program that checks finds the lie. Each operation's
  **trick surface** — the syscalls, `/proc` files and libc accessors that
  expose it — must be intercepted together.

## Two kinds of hard limit

Not every limit fails the same way, and the difference decides the response.

- **Forbidden outright.** The operation can produce none of its effect. The
  call fails (`EPERM`/`EINVAL`/`ENOSYS`) or the process is killed (`SIGSYS`).
  Examples: user namespaces, `mount`/`pivot_root`, `io_uring`, keyrings. A
  failing call is a signal the caller can see, so it can be emulated (made a
  convincing success) or refused cleanly (below).
- **Restricted or divergent.** The call runs, but not as a stock Linux system
  would: allowed only for some argument values, or only for some
  sub-operations, or it succeeds with a different scope, or it reports the
  platform's own values. Examples: ports below 1024 (`EACCES`, while higher
  ones bind), raw sockets (protocol-dependent), partial `/proc` visibility
  (`/proc/cpuinfo` reads, `/proc/version` denied), `uname`/cgroup/`rlimit`
  reporting Android, `mprotect(PROT_EXEC)` on an existing mapping (W^X), and
  SysV IPC (`shmget` works, `msg*`/`sem*` do not). **No error is raised**, so
  the program trusts a wrong answer: this class must be **normalized** to the
  behaviour a package expects, not merely answered.
- **Forbidden with a fallback.** A sub-case of divergent: the exact syscall is
  missing, but libc already has a documented older one (`clone3`→`clone`,
  `statx`→`fstatat`, `openat2`→`openat`). Benign — route to the older variant
  and the program never notices.

The first class asks "emulate or refuse?"; the second asks "what does the
package assume, and how do we make the observation match it?". The second is
the more dangerous one precisely because it is silent, which is why
[Consistency](#consistency-the-real-work) is the bulk of the work.

Whichever class, the tier that handles it is chosen by how the call reaches
the kernel, not by the class itself:

| | forbidden outright | restricted / divergent |
| --- | --- | --- |
| surfaces as | a syscall/probe failure, or `SIGSYS` | a libc accessor value (`uname`, `sysconf`, `stat` of `/proc`, `mprotect`) — no error |
| primary tier | dn-trace (catches `SIGSYS`, fakes `mount`/`unshare`) | dn-glibc (fast path, native speed) |
| dn-trace's role | near-mandatory — only the tracer sees Gate A | backstop for what libc cannot see |

The direction: the model and the normalize-or-refuse decision live in
dn-policy; dn-glibc applies it to the ordinary libc surface, dn-trace to
everything libc cannot reach (static, Go, raw `svc #0`, foreign loaders, and
every Gate-A event). Neither tier alone is enough — normalizing only in
dn-trace leaves fast-path dynamic binaries with the wrong answer; only in
dn-glibc lets static/Go programs through unnormalized.

## Already emulated

| operation | today | where |
| --- | --- | --- |
| `mount`/`umount`/`pivot_root` | recorded in a binding table, returns 0 | `src/tracer/syscall/enter.c:489,473,520` |
| `unshare`/`setns` | returns 0 | `src/tracer/tracee/seccomp.c:251` |
| `setgroups`/`set*id` | success; the owner is recorded | `seccomp.c`, `src/dn-policy/dn-policy-fakeroot.c` |
| `statfs` of `/dev/shm` | reported as tmpfs | `src/tracer/tracee/seccomp.c:359` |
| AF_NETLINK `NETLINK_ROUTE` | substituted with AF_UNIX/SOCK_DGRAM | `src/tracer/syscall/enter.c:2084` |
| `chown` / `stat` owner | owner store (`user.dn.*` xattr) | `dn-policy-fakeroot.c` |

These are the template: each fakes one interface and answers all its readers
from one model.

## Virtual filesystem

What Android refuses: overlayfs, `mount`, `/dev/fuse`.

- **Overlay → userspace copy-up.** Instead of an overlay mount, a write that
  lands on a lower (read-only) layer copies the file into the writable store
  first, then redirects; a delete leaves a whiteout; a directory read merges
  the layers. This reproduces overlay semantics without the kernel feature.
- **Mount table.** `mount`/`umount` update the table; `statfs`/`statvfs`/
  `statx` read it; the `/proc` mount files are synthesized from it (below).
- **`/dev/shm`.** No such directory exists and none can be created; serve it
  from a writable store, with `statfs` reporting tmpfs (the fstype half is
  already done).

Trick surface: `mount`, `umount2`, `pivot_root`, `statfs`/`statvfs`, `statx`,
`readdir`, `open`/`access` of `/proc/*/mounts`, `link`/`rename` across layers.

## Synthetic /proc and /sys

What SELinux refuses: reads of `/proc/sys/*`, `/proc/net/*`, `/proc/version`
— and a fresh `/proc` mount does not lift it
([`known-issues.md`](../reference/known-issues.md), "Gaps toward a complete
Debian").

Treat a set of paths as **virtual files**: a `path -> generator` table in
dn-policy, with `openat`/`stat`/`read` served from it. `/proc/self/*` derives
from the tracee's own state (pid, uid, capabilities, mounts, fds);
`/proc/version`, `cpuinfo`, `meminfo`, `/proc/sys/*` return plausible
constants. Precedents: `src/tracer/path/proc.c`, the `statfs` fake, and the
`priv/getent` wrapper that already answers the account databases from the
prefix's files.

Trick surface: every open of the path plus `stat`/`access`/`readlink` of it —
an answer that exists for `open` but not `stat` is a leak.

## Network

What Android refuses: raw sockets, privileged ports, TUN.

- **Raw ICMP.** Substitute the unprivileged ping socket
  (`SOCK_DGRAM, IPPROTO_ICMP`) where the kernel allows it, else a userspace
  helper — the same shape as the AF_NETLINK substitution.
- **Privileged ports.** Translate a `bind()` on a port below 1024 to an
  unprivileged one and keep the mapping; translate it back in `getsockname`/
  `getpeername`/`accept`/`connect`; synthesize `/proc/net/tcp` with the
  requested port.
- **TUN/firewall.** Not convincingly fakeable; refuse with a reason.

Trick surface: `socket`, `bind`, `getsockname`, `getpeername`, `accept`,
`connect`, `/proc/net/{tcp,tcp6,udp,unix}`, `SIOCGIF*` ioctls.

## Identity and capabilities

`setuid`/`setgid` already fake success with an owner store. Extend the same
idea to **capabilities**: `capget` (already in the syscall catalog) returns a
plausible set, `prctl(PR_CAPBSET_READ)` returns 1, `capset` returns 0. A
program that checks for a capability it was denied then proceeds.

Trick surface: `stat` owner, `/proc/self/status` (`CapEff`, `Uid`),
`getuid`/`geteuid` and the rest of the identity group.

## Namespaces and pid

`unshare`/`setns` already return 0. Complete the illusion with virtual
`/proc/self/ns/*` links and a consistent pid view wherever a program reads
one. No real isolation exists — this is presentation only.

## Consistency: the real work

Feasibility is not the problem; coherence is. The engineering cost is a single
model every reader consults, and the discipline that no new faker is added
without covering its whole trick surface. The invariant to hold: for any
operation a model claims, no interface is left that reports the unfaked truth.

## Refuse, don't fake

Faking security-relevant state is a footgun: a program that trusts a fake
capability or LSM state can make a wrong decision. Answer these with a clear
refusal (the "clear refusals" item in [`../../TODO.md`](../../TODO.md)), not a
fiction: real isolation, TUN, `SO_PEERSEC`/`SO_*SEC` and other LSM-observable
state, kernel keyrings, self-restriction (`landlock`, `seccomp`), and anything
whose correctness a program relies on for safety.

## Priorities

1. Extend the existing precedents (mount table, netlink, `statfs`, owner
   store).
2. Cheap and common: `capget`, `/proc` synthesis, privileged-port translation,
   `/dev/shm`.
3. Stretch: overlay copy-up with merged reads.
4. Refuse: TUN, isolation, LSM state.
