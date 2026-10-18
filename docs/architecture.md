# Architecture

How a Debian program runs in a prefix on Android, unprivileged: what is caught,
where it is answered, and what it costs. Building and booting a prefix is in
[`building.md`](building.md); the measurements are in
[`performance.md`](performance.md). The model it serves -- the Android kernel
mapped into the prefix -- is [`spec/mapped-kernel.md`](spec/mapped-kernel.md);
what a program may rely on, at what fidelity and cost, is
[`spec/contract.md`](spec/contract.md).

## Contents

- [The problem](#the-problem)
- [Two parts: the tracer and the shim](#two-parts-the-tracer-and-the-shim)
- [The tracer: dn-tiny](#the-tracer-dn-tiny)
- [The shim: dn-lane.so](#the-shim-dn-laneso)
- [The law: guest paths and host paths](#the-law-guest-paths-and-host-paths)
- [Identity: fake root](#identity-fake-root)
- [What Android refuses](#what-android-refuses)
- [The boundary: what a program sees](#the-boundary-what-a-program-sees)
- [Inherited from Android](#inherited-from-android)
- [Known gaps](#known-gaps)
- [The core and the variants](#the-core-and-the-variants)

## The problem

A Debian program expects its files at `/usr`, `/etc`, `/var`, its loader at
`/lib/ld-linux-aarch64.so.1`, root to install packages, and a kernel that lets it
make hard links, open an audit socket or register `rseq`. An Android app has
none of that: its files live under `/data/data/APP/...`, it has no root, no
`chroot`, no namespaces, a seccomp filter that kills some syscalls, and SELinux
rules that refuse others.

deb-native installs Debian's packages unmodified into a directory (the prefix)
and makes each program see that directory as `/`, by changing what its syscalls
say, never what is on disk.

## Two parts: the tracer and the shim

```
a glibc program in the prefix
 ├── its libc calls (open, stat, realpath, getuid, ...) ─► dn-lane.so, in process
 ├── libc's own calls (opendir, fopen, locale, ...)     ─► dn-lane.so, through libc's
 │                                                         patched svc sites
 └── everything else, and every static program         ─► the kernel, through
                                                            dn-tiny's seccomp filter:
                                                            most calls pass, the
                                                            ones that need the law
                                                            stop in dn-tiny
```

- **`dn-tiny`** (`src/dn-tiny/`, about 2,000 lines, no libc) is the tree's root
  process. It starts every program under `ptrace` with a seccomp filter and
  handles the syscalls that need a path mapped, an identity faked or an answer
  the platform will not give. It works for any program, static or dynamic.
- **`dn-lane.so`** is preloaded into every glibc program `dn-tiny` starts. It does
  the same path work inside the process, for the calls that only read, so they
  cost no stop. A call the shim does not take still reaches `dn-tiny`: the shim
  makes things faster, never different.

A stop in the tracer costs 30-600 us on the Android 12 phone, most of it the
kernel waking a sleeping core; the same call answered in the shim costs about
1 us. The shim takes the frequent calls; the tracer keeps the rare ones and is
the net for everything.

## The tracer: dn-tiny

- **The filter.** A classic BPF program built from the policy's rows
  (`src/dn-tiny/gen_rows.py` generates them): a syscall is caught only when the
  tracer does something with it, and only a row whose result is rewritten asks
  for the exit stop. One JGE tree holds every row: a number no row lists is a few
  comparisons from `ALLOW`, and a row the lane also lists tests the cookie in its
  own leaf. Everything else is `ALLOW`. A few answers need no tracer at all: a
  `NETLINK_AUDIT` socket gets `EPROTONOSUPPORT` from the filter itself.
- **The lane.** A call carrying the lane cookie in `x5` (`src/dn-tiny/lane.h`)
  passes the filter without a stop, if it is one of the shim's read-only calls
  (`readlinkat`, `newfstatat`, `fstat`, `faccessat`, `statx`, the `getxattr` and
  `listxattr` pairs, `getcwd`, and `openat` without a flag that writes). Mutating
  calls always reach the tracer, so its caches stay right.
- **Answers at entry.** Where the result is only data (`readlinkat`,
  `newfstatat`, `statx`, the identity calls), the tracer makes the call itself on
  the host path, fixes the result and skips the syscall: no exit stop.
- **The exec gate.** An `execve` of a glibc program is rewritten to run the
  program's own `PT_INTERP` -- the tree's loader, whatever it is called
  (`/lib/ld-linux-aarch64.so.1`, `/lib64/...`) -- as `loader --preload dn-lane.so
  --argv0 NAME PROGRAM`. The loader, and the shim it preloads (the runtime's
  own), are host paths; the program is **the tree's own** path, so the loader
  opens it through the law like any program, and the loader finds the libraries
  through the tree's `ld.so.cache` (their paths are the tree's too). Scripts go
  through their interpreter the same way. `/proc/<pid>/exe` is answered with the
  program, not the loader.
- **Per-session state**: the fake ids of each process, the program each process
  runs, the working directory, a cache of `readlink` answers whose generation
  lives in `var/lib/deb-native/dn-tiny.gen` (shared by every session on the
  prefix; any link change moves it), and the copies made for hard links.
- **`-s USEC`** polls for the next stop before sleeping. A sleeping tracer lets
  its core drop to a low clock, and every stop pays to wake it.

## The shim: dn-lane.so

- **Symbols.** It defines `open`, `openat`, `stat`, `lstat`, `fstatat`, `fstat`,
  `statx`, `access`, `faccessat`, `euidaccess`, `readlink(at)`, `realpath`,
  `getcwd`, the `getxattr`/`listxattr` pairs, the `get*id` calls and the `set*id`
  calls, `link(at)` and `unlink(at)`. Each maps its path through the one law
  (`policy.c`, linked into the shim with `-DDN_LIBC`) and makes the call on the
  lane; a case it does not handle (a write, an unknown flag, a deleted
  directory's fd) goes to libc and the tracer.
- **libc's own calls.** At load the shim scans `libc.so.6`'s text for `mov x8,
  #nr; svc #0` with an `nr` it answers and replaces each `svc` with a branch to a
  stub within 120 MB. The stub saves every register and the flags, calls the hook
  (which reads `x8` at run time, keeps `errno`, and answers like the kernel), or
  makes the original call. libc's cancellable path (`__syscall_cancel_arch`) is
  left alone, since `pthread_cancel` checks the program counter there. The stock
  Debian `libc6` stays as it is on disk; `DN_LANE_PATCH=0` turns this off.
- **Copies, kept safe.** The law's cache of `readlink` answers (`policy.c`; the
  shared generation drops it), the working directory and the ids are seqlocks,
  the last two tagged with the pid they were asked for: a `vfork` child shares
  its parent's memory.
- **The cookie.** The shim issues its own `svc` with the cookie in `x5` and
  clears `x5` after it, so a later call libc makes by itself never carries it.

## The law: guest paths and host paths

- A guest path is the prefix's: `/usr/bin/ls` is `ROOT/usr/bin/ls`.
- The mapping is one **table** (`policy.c`): a row per guest path. `/` is the
  prefix root; three host directories stay the phone's under their own names
  (`/proc`, `/sys`, `/dev`); each bind in a prefix's `boot/binds` is a row too.
  Guest to
  host by the longest guest row, host to guest by the longest host row.
- The path is walked one component at a time on the **guest** path, links and
  `..` resolved there (an absolute link target is the tree's, `/etc/alternatives/
  awk -> /usr/bin/mawk`; `..` at a bind is the tree's parent), and its host path
  taken at the end. What the kernel returns as a host path (`getcwd`, a link
  target, `/proc/self/fd/N`) is mapped back to the guest's.
- A call on a real directory fd: the guest path of the directory the fd names,
  then the name from there; a host directory's paths stay.
- A bind's mount point is an empty directory `dn-tiny` makes at boot; a `rename`
  or `link` between two rows answers `EXDEV`, as between two mounts; a write to a
  `ro` row answers `EROFS`.
- The mount view is the table too: `/proc/mounts`, `/proc/self/mounts`,
  `mountinfo` and `/etc/mtab` are rows that map straight to a file built at boot
  under `var/lib/deb-native` (a memfd cannot cross the ptrace boundary), so the
  reads need no stop and the source shown is `deb-native`, never a host path.
- An AF_UNIX socket path is a path: `bind`, `connect`, `sendto` and `sendmsg`
  carry it in a `sockaddr_un`, and `getsockname`, `getpeername`, `accept`,
  `recvfrom` and `recvmsg` fill one; the tracer maps `sun_path` guest<->host at
  entry and exit (the length with it).  On the ADB host the `shell` domain may
  not create a `sock_file` in the tree, so the guest path becomes an abstract
  name (`\0dn-<hash>` of the root and the host path) and the answer is mapped
  back; bind also makes a regular placeholder at the path, so `stat`, `chmod`
  and `unlink` work (`stat` reports a file, not `S_IFSOCK`).
- `/dev/shm` is the tree's, not the host's: the law maps it to `ROOT/run/shm`
  (created 1777 by `init.sh`), so `shm_open` works as files on both hosts even
  though the phone's `/dev` has no writable shm.
- The law lives once, in `policy.c`, and both engines read it: the tracer links
  it as it is, the shim builds it `-DDN_LIBC` (the types, the strings and
  `pol_readlink` from libc and the lane), the loader hook builds it `-DDN_FREE`
  (own strings). See `spec/plan.md`, Phase 0.

## Identity: fake root

- There is one real identity (the app's uid/gid); every other is virtual.  The
  boot seeds the first process with the **real uid** by default
  (`DN_INITIAL=root` seeds a root session), and every syscall is classified by
  the process's current **euid**: root (0) gets the fake-root view, a service
  uid its fake identity without root, the app uid the kernel's own answer.
  There is no mode flag (see [`spec/identity.md`](spec/identity.md)).
- `set*id` is emulated when the euid is 0 and answers `EPERM` otherwise; `chown`
  is stored (`user.dn.owner`, 0:0 included) only by root, and a create is marked
  with the creator's ids.  A file with no store -- one that predates marking,
  or a symlink, which takes no user xattr -- reads `0:0` in both worlds, so the
  root session and the real user cannot disagree about it.
- `access()`/`faccessat` answer the **world's** view, not the kernel's: root
  passes (still needing an execute bit for `X_OK`), anyone else is judged by the
  stored owner and the virtual group list.  The shim answers the clear cases in
  process; the tracer answers the rest.
- A **setuid-bit exec** is the gate: a stored owner 0 gives euid 0, a stored
  owner the real uid gives euid the real uid (no store is root's); a sudo-like
  helper then calls `setgid(0)`/`setuid(0)`
  to make the command root.  The gate hands the shim its ids in `DN_IDS`.
- Programs that expect root's kernel powers are answered, not granted: a
  fake-root write open temporarily adds the owner write bit (restored at the
  exit stop), `NoNewPrivs` reads 0, `PR_SET_DUMPABLE` is ignored, and the
  tracer reaches a non-dumpable tracee through `PTRACE_PEEKDATA`/`POKEDATA`.
  Real `sudo`, `su` and `passwd` run this way (the test prefix's
  `/etc/sudoers.d/user` is NOPASSWD); the kernel still sees the app.
- `/proc/<pid>/status` says the process's ids (root, service or real), so a
  reader that never calls `get*id` sees the same identity; supplementary groups
  are a per-process virtual list (`{0}` for root, the app's groups for the real
  user at boot, replaced by `setgroups`/`initgroups`).
- The per-pid state (ids, groups, the program name, the exit-stop flags) is one
  struct keyed by the **full pid** in a 4096-bucket hash, with 1024-state blocks
  and a free list; a one-entry cache keeps the common same-pid stop cheap.  The
  old 65536-slot table let two live pids 65536 apart share state (this phone's
  pid_max is 4194304); the startup warning is gone with it.
- The goal is no accidental identity leak (apt, dpkg and `git`'s ownership check
  see what the tree's user expects), not isolation.

## What Android refuses

| refused | what the program sees |
| --- | --- |
| hard links (`linkat`: SELinux) | a copy of the file; the copies of one file report their count as `st_nlink` (the tracer for the session, the shim for a process's own links), so shadow's lock works |
| `NETLINK_AUDIT` sockets | `EPROTONOSUPPORT`, as from a kernel without audit (libaudit then carries on) |
| `rseq`, `set_robust_list`, `clone3`, `faccessat2` (the app's seccomp filter) | answered at the `SIGSYS` stop: `ENOSYS`, or the older call substituted; `init.sh` turns glibc's `rseq` off |
| `mount`, `umount2`, `swapon`, `swapoff` | answered at the `SIGSYS` stop |
| locales other than `C.UTF-8` | not installed; `init.sh` links `C.UTF-8` to `C.utf8` so glibc's first try succeeds |

## The boundary: what a program sees

A program in the prefix sees three things, and nothing else of Android:

- the prefix, as `/`;
- the kernel's own truth: `/proc`, `/sys`, `/dev`, `uname`, as a Debian chroot
  on the same kernel would see them;
- the host directories the law lets into the tree (today `/proc`, `/sys` and
  `/dev` under their own names; a bind can add another on purpose).

The test for a leak: a program that does not ask where it runs and still meets
Android (in `ls -l`, in a leaky `/proc` file) is a bug. A program
that looks on purpose finds out, and that is accepted:

- `uname`, `/proc/version`, `/proc/cpuinfo`: the phone's kernel;
- `/proc/self/status`: `TracerPid` is `dn-tiny`, `Seccomp` is 2;
- `/dev`: `binder`, `ashmem`;
- SELinux: `/sys/fs/selinux`, and an `EACCES` where Debian would allow;
- the time a call takes when it stops in the tracer.

So the runtime answers in `/proc` what ordinary tools read: the mount view, the
process view (`cmdline`, `comm`, `status` -- its `TracerPid`/`Seccomp`, the
program name, and the fake root's `Uid`/`Gid`), `maps`/`smaps`, and the links
already told back (`cwd`, `exe`, `fd`). The kernel's own truth is left alone:
`uname`, `/proc/version`, `cpuinfo`.

This is about accidental leaks only, not a sandbox. A program can always reach
the host: it can issue a syscall with the lane cookie, or use a path the law
leaves to the host.

**One prefix, one boot.** A prefix is booted by its host (an app, Termux), never
from inside another prefix: a seccomp filter cannot be removed, so a tree
started under another tree's `dn-tiny` would carry that tree's filter. Several
prefixes can run at once, each under its own `dn-tiny`; they share nothing.

## Inherited from Android

The runtime maps the Android kernel, it does not emulate one: the kernel a
program reaches is the phone's, real. The boundary above lists what that leaks
by accident; the same rule is an opportunity to take on purpose. Everything the
kernel adds over a stock Debian's is there to be reached -- `binder` and
`ashmem` (Android's IPC and shared memory, with no Debian analog), the GPU nodes
`/dev/kgsl-3d0` and `/dev/ion`/`/dev/dma_heap` ([`../TODO.md`](../TODO.md),
"Graphics"), the Android netlink families. A Debian on a PC has none of them;
here they are a bind away (`boot/binds`) and an `ioctl` that is never stopped. It
is the flip side of "the kernel is the truth" (`spec/phase2.md`): the rule that
shows the phone's `uname` and `cpuinfo` shows its devices too, and a program
that wants one finds it. (In the app domain SELinux may still refuse a node;
whether to expose it is the host's decision, a bind and a grant, not the
runtime's.)

## Known gaps

- The copies made for hard links do not share their contents.
- A program without the shim (static, Go) sees the real owner through `fstat`
  only, and the real link count of a copy.
- `name_to_handle_at` is not mapped; `openat2` with `RESOLVE_*` answers `ENOSYS`
  (a clear refusal, not a hole).
- `/proc/<pid>/maps` and `smaps` are generated at open, the file paths mapped to
  the tree's (`synth_maps`).
- The kernel's own refusals stay: SELinux answers `EACCES` where Debian would
  allow (`/sys/fs/selinux`, parts of `/sys`), and `PPid`/`getppid()` name the
  tracer (a structural residue; `docs/spec/phase2.md`, "An accepted residue").
- `ld.so`'s own opens (the program, the shim, the libraries) are removed by the
  loader hook when a prefix ships it (`--ldhook`); without it they stop in the
  tracer.

## The core and the variants

The runtime is the **core**: `dn-tiny`, the shim, and the law. It is the
invariant, and it knows nothing of dpkg, apt or Debian -- a tree with a loader,
its libraries and a program is enough ([`spec/mapped-kernel.md`](spec/mapped-kernel.md)).
A **variant** is a tree and its boot glue. The Debian a `build-prefix.sh` makes
is the reference, and a busybox image, a single-program bundle or a "bare app"
are others on the same axis: the core takes no part in choosing one and asks
none of them what it is. What the variant decides is whether there is a shim (a
static tree, busybox or Go, needs only `dn-tiny`) and how it boots. Two places
of the core still lean Debian -- the loader named in `policy.c` and `boot.sh`'s
`/usr/bin/bash` -- and both are open (`TODO.md`, "Beyond Debian, and bundles").
What the app around a prefix supplies -- the session, the boot inputs, the
Android bridge, display and input, the services -- is the **host contract**
([`spec/host.md`](spec/host.md)).  The host is a separate axis from the tree:
Termux, a custom app or ADB, with the app or adb runtime
([`spec/hosts.md`](spec/hosts.md)).
