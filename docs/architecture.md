# Architecture

How a Debian program runs in a prefix on Android, unprivileged: what is caught,
where it is answered, and what it costs. Building and booting a prefix is in
[`building.md`](building.md); the measurements are in
[`performance.md`](performance.md). The model it serves -- the Android kernel
mapped into the prefix -- is [`spec/mapped-kernel.md`](spec/mapped-kernel.md).

## Contents

- [The problem](#the-problem)
- [Two parts: the tracer and the shim](#two-parts-the-tracer-and-the-shim)
- [The tracer: dn-tiny](#the-tracer-dn-tiny)
- [The shim: dn-lane.so](#the-shim-dn-laneso)
- [The law: guest paths and host paths](#the-law-guest-paths-and-host-paths)
- [Identity: fake root](#identity-fake-root)
- [What Android refuses](#what-android-refuses)
- [The boundary: what a program sees](#the-boundary-what-a-program-sees)
- [Known gaps](#known-gaps)

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
  prefix's loader: `ld-linux-aarch64.so.1 --library-path ... --preload
  dn-lane.so --argv0 NAME PROGRAM`. The loader it execs, and the shim it
  preloads (the runtime's own), are host paths; the program and the library
  search path are **the tree's own** paths, so the loader opens them through the
  law like any program. Scripts go through their interpreter the same way.
  `/proc/<pid>/exe` is answered with the program, not the loader.
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
- Ten host directories stay the phone's: `/proc`, `/sys`, `/dev`, `/data`,
  `/system`, `/storage`, `/sdcard`, `/apex`, `/vendor`, `/mnt`.
- Links are resolved component by component: an absolute link target is the
  prefix's too (`/etc/alternatives/awk -> /usr/bin/mawk`).
- What the kernel returns as a host path inside the prefix (`getcwd`, a link
  target, `/proc/self/fd/N`) is told back as the guest's.
- A call on a real directory fd resolves the name from the directory's guest
  path (the host path the kernel names for the fd, minus the root); a directory
  outside the prefix is a host directory, and its paths stay.
- The law lives once, in `policy.c`, and both engines read it: the tracer links
  it as it is, the shim builds it `-DDN_LIBC` (the types, the strings and
  `pol_readlink` from libc and the lane). See `spec/plan.md`, Phase 0.

## Identity: fake root

- Every process starts as root (uid and gid 0). `set*id` calls are accepted and
  remembered per process, inherited by children; `get*id` say what was set.
- Files owned by the app's uid read as owned by root in every `stat` the tracer
  or the shim answers. `chown` is accepted and not kept.
- The goal is no accidental identity leak (apt, dpkg and `git`'s ownership check
  see a consistent root), not isolation.

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
- the host directories the law lets into the tree (today the ten above, under
  their own names).

The test for a leak: a program that does not ask where it runs and still meets
Android (in its environment, in `ls -l`, in `df`, in `pwd`) is a bug. A program
that looks on purpose finds out, and that is accepted:

- `uname`, `/proc/version`, `/proc/cpuinfo`: the phone's kernel;
- `/proc/self/status`: `TracerPid` is `dn-tiny`, `Seccomp` is 2;
- `/dev`: `binder`, `ashmem`;
- SELinux: `/sys/fs/selinux`, and an `EACCES` where Debian would allow;
- the time a call takes when it stops in the tracer.

So what the runtime answers in `/proc` is what ordinary tools read (`mounts`,
`mountinfo`, `maps`, and the links already told back: `cwd`, `exe`, `fd`), not
`status` or `cpuinfo`.

This is about accidental leaks only, not a sandbox. A program can always reach
the host: it can issue a syscall with the lane cookie, or use a path the law
leaves to the host.

**One prefix, one boot.** A prefix is booted by its host (an app, Termux), never
from inside another prefix: a seccomp filter cannot be removed, so a tree
started under another tree's `dn-tiny` would carry that tree's filter. Several
prefixes can run at once, each under its own `dn-tiny`; they share nothing.

## Known gaps

- `chown` is not kept: a file's owner reads as root or as its real uid's
  number, never as another user.
- The copies made for hard links do not share their contents.
- A program without the shim (static, Go) sees the real owner through `fstat`
  only, and the real link count of a copy.
- `execveat`, `openat2` with `RESOLVE_*`, `name_to_handle_at`, AF_UNIX socket
  paths (`bind`, `connect`) are not mapped yet.
- `ld.so`'s own opens (the program, the shim, the libraries) happen before the
  shim is loaded and stop in the tracer.
- Where the boundary leaks without being asked (`TODO.md`, "The boundary"): the
  host's environment passes through (`ANDROID_*`, `BOOTCLASSPATH`, the host's
  `PATH`-like variables), and so do its umask (077) and, for a command, its
  working directory; `ls -l` shows the SELinux label as a `.` after the mode;
  `/proc/self/mounts` is Android's; `/data`, `/storage`, `/sdcard`, `/system`
  and the other host directories answer `stat` and `cd`; `/proc/self/maps`
  names the libraries by their host paths. There is no way to bind a host
  directory at a guest path of one's choosing.
