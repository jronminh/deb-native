# Device census: which syscalls the app domain may call

A measurement of one device and one execution context, with the method, so it
can be repeated on another phone. It answers a narrow question: which syscalls
kill a process in the app's own domain under Android's seccomp filter. Taken by hand;
the table is one device, not a general policy. What the runtime answers for the
calls killed here: [`architecture.md`](../architecture.md), "What Android
refuses".

## Contents

- [Method](#method)
- [Context](#context)
- [Result](#result)
- [What it means for glibc programs](#what-it-means-for-glibc-programs)
- [Open items](#open-items)

## Method

`src/dn-tiny/census.c`, a freestanding aarch64 program (no libc, 88 lines, 2.5 KB;
built by `scripts/build/build-tiny.sh` as `dn-census`) forks, with a raw
`clone`, once per syscall; the child makes the call with zero arguments and exits
with the errno, and the parent reports how the child ended. Zero arguments make
the call fail harmlessly (`EFAULT`, `EINVAL`). A death by signal 31 is the
seccomp filter's answer to a call it does not allow. The first run tried 62 numbers (an earlier version that took a list; the program in
the tree tries every number from 0 to 470, see "The whole range"), 
including a number no kernel defines (9999) and `getpid` as a control. It was run
twice: in the app's own process tree, and through `run-as`, whose shell has
`Seccomp: 0`.

## Context

Samsung SM-G970W, Android 12, kernel 4.14.190, app `org.dn.shell.dev`
(`targetSdk 28`), SELinux context `u:r:untrusted_app_27:s0`, `Seccomp: 2`, cpuset
`/top-app`, 2026-10-13. Through `run-as` none of the 62 killed the process; in the
app 32 did.

## Result

**Killed with `SIGSYS` in the app** (32 of 62; `run-as` returned `ENOSYS`,
`EPERM`, `EFAULT` or `EINVAL` for each):

| group | syscalls |
| --- | --- |
| glibc startup and threads | `rseq`, `set_robust_list`, `get_robust_list`, `clone3` |
| newer variants glibc tries first | `faccessat2`, `close_range`, `openat2`, `fchmodat2`, `epoll_pwait2`, `futex_waitv` |
| identity | `setuid`, `setgid`, `setgroups` |
| mount and root | `mount`, `umount2`, `chroot`, `swapon` |
| SysV IPC and queues | `shmget`, `shmat`, `msgget`, `semget`, `mq_open` |
| memory policy | `mbind`, `get_mempolicy`, `set_mempolicy` |
| handles, comparison, rings, sandboxing | `name_to_handle_at`, `open_by_handle_at`, `kcmp`, `io_uring_setup`, `io_uring_enter`, `landlock_create_ruleset` |
| a number no kernel defines | 9999 |

**Not killed** (an errno or success, the same as `run-as` or close to it):
`statx`, `pidfd_open`, `pidfd_getfd`, `pidfd_send_signal`, `process_vm_readv`,
`process_vm_writev`, `memfd_create`, `userfaultfd`, `unshare`, `setns`,
`pivot_root`, `mknodat`, `setresuid`, `capset`, `sched_setaffinity`, `getrandom`,
`copy_file_range`, `renameat2`, `linkat`, `symlinkat`, `membarrier`, `futex`,
`epoll_create1`, `eventfd2`, `timerfd_create`, `inotify_init1`, `pipe2`, `socket`,
`perf_event_open`, `getpid`. `setresuid` returns `EPERM` while `setuid` is killed,
so the identity family is not uniform.

Two things follow from the control rows. A number the filter does not list is
killed like a denied one (the filter is default-deny), and the filter acts
by killing, not by returning an error, which is why a program that would fall
back on `ENOSYS` dies instead.

## The whole range

The same method over every number from 0 to 470 (457 of them: 14 are skipped
because they end or hang the process, stop it under a tracer, or change global
state), once in the app and once through `run-as`:

| | count |
| --- | --- |
| allowed in the app (any result, including an error) | 201 |
| killed with `SIGSYS` in the app | 256 |
| of those, calls this kernel implements (`run-as` does not say `ENOSYS`) | **32** |
| of those, numbers this 4.14 kernel does not have (`run-as` says `ENOSYS`) | 224 |

The 224 matter only on a newer kernel, where many are real calls: `rseq`,
`clone3`, `close_range`, `openat2`, `faccessat2`, `fchmodat2`, `epoll_pwait2`,
`io_uring_*`, `futex_waitv`, `landlock_*`, the SysV and POSIX message queue calls,
`mbind` and its relatives, `kcmp`, the `*_handle_at` pair; the rest are unassigned
numbers. A runtime answers all of them `ENOSYS`, which is the truthful answer for
a call the system does not offer.

The 32 are the real denials on this device, and what the runtime does with each:

| answer | count | calls |
| --- | --- | --- |
| the same call by an equivalent: `accept` is `accept4` with no flags | 1 | `accept` |
| succeed, as root would | 8 | `setuid`, `setgid`, `setreuid`, `setregid`, `setresgid`, `setfsuid`, `setfsgid`, `setgroups` |
| refuse with `EPERM`, as for any non-root user | 17 | `mount`, `umount2`, `chroot`, `swapon`, `swapoff`, `clock_settime`, `settimeofday`, `adjtimex`, `clock_adjtime`, `sethostname`, `setdomainname`, `syslog`, `init_module`, `delete_module`, `finit_module`, `lookup_dcookie`, `bpf` |
| `ENOSYS`: no equivalent, and programs tolerate it | 6 | `set_robust_list`, `get_robust_list`, `remap_file_pages`, `add_key`, `request_key`, `keyctl` |

So 9 of the 32 are restored in full. Three of the 17 (`mount`, `umount2`,
`chroot`) could be restored through the path mapping, by changing the root or
adding a mapping for that process, as PRoot does; nothing else of the 17 can be
restored without privilege. `setresuid` is not killed but answered `EPERM`, so it
is faked at entry like the identity calls. Of the 224, three have an equivalent
that is implemented (`faccessat2`, `fchmodat2` and `openat2` to the older call),
and two more could have one (`epoll_pwait2` to `epoll_pwait`, `clone3` to `clone`);
glibc already falls back on `ENOSYS` for both.

## A second device: Android 16

The same census on the owner's main phone, Samsung SM-S7110 (Galaxy S23 FE),
Android 16 (SDK 36), kernel 5.10.240, 4 KB pages, with the dev app
`org.dn.shell.dev` installed for the purpose (`run-as`'s shell and `adb shell`
both show `Seccomp: 0`, so the census runs inside the app's own process tree;
`Seccomp: 2`, `untrusted_app_27`, cpuset `/top-app`, 2026-10-13). As a control,
the shell domain kills none of the 457 numbers.

| | Android 12, kernel 4.14 | Android 16, kernel 5.10 |
| --- | --- | --- |
| allowed in the app | 201 | 217 |
| killed with `SIGSYS` | 256 | 240 |
| of those, the kernel implements the call | 32 | 42 |
| of those, the kernel lacks the call | 224 | 198 |

What changed:

- **Allowed on Android 16, killed on Android 12**: `faccessat2`, `openat2`,
  `clone3`, `close_range`, `fchmodat2`, `epoll_pwait2` and `bpf`. The first four
  run natively on this kernel (which implements them); `fchmodat2` and
  `epoll_pwait2` answer `ENOSYS`, since the kernel lacks them.
- **Killed on Android 16, not on Android 12**: nothing that was allowed
  before. The additions to the list of real denials are calls this kernel now has:
  `io_uring_setup`, `io_uring_enter`, `io_uring_register`, the new mount API
  (`open_tree`, `move_mount`, `fsopen`, `fsconfig`, `fsmount`, `fspick`), `kcmp`
  and `io_pgetevents`.
- **Unchanged**, and what a glibc process needs answered: `rseq`,
  `set_robust_list`, `get_robust_list`, `accept`, the `set*id` family and
  `setgroups`, `mount`, `umount2`, `chroot`, `swapon`, `swapoff`, the clock and
  hostname setters, the module calls, the keyring calls, `remap_file_pages`, the
  SysV and POSIX queue calls, the memory-policy calls.

Stock Debian glibc (`libc6 2.41-12+deb13u4`, the loader and libraries as they come
out of the `.deb`) started through `ld.so` in the app, with no tracer, was killed
by `SIGSYS` with and without `GLIBC_TUNABLES=glibc.pthread.rseq=0`, and ran under
`dn-tiny` with `rseq` and `set_robust_list` answered once for each process. The
stock files were not modified.

## What it means for glibc programs

- Every glibc process, static or dynamic, calls `rseq` and `set_robust_list` at
  startup. In this domain both kill it. `GLIBC_TUNABLES=glibc.pthread.rseq=0`
  removes only the first: a static glibc binary was killed three times out of
  three with the tunable set. The outermost process (the tracer, which nothing
  traces) therefore has no glibc startup at all: `dn-tiny` has no libc.
- glibc 2.33 and later try `faccessat2` first for `access` and `faccessat`, and
  `clone3` for threads and `posix_spawn`, and fall back only on `ENOSYS`. Here they
  are killed, so a program that makes those calls dies unless something answers
  `ENOSYS`.
- A freestanding binary (this one) starts and runs in the app, since it never
  makes the startup calls.

## Open items

- Two devices, one Android version each. Samsung may change the policy between
  releases of the same Android version.
- The census reports only the death; that the action is `SECCOMP_RET_TRAP` (a
  catchable `SIGSYS`) is known from `dn-tiny` answering these calls at the signal
  stop.
- 62 numbers is a sample, not the whole table.
