# Two gates: seccomp and the kernel

Why a call fails in the prefix is not one question.  A call passes the app's
**seccomp** filter, then the **kernel**; either can refuse it, and the two are
independent.  This page separates them, from the AOSP sources and the device
census ([`device-census.md`](device-census.md)); the promises that follow are in
[`../spec/contract.md`](../spec/contract.md).

## The seccomp gate

bionic's app policy is generated as **`SYSCALLS.TXT - blocklist + allowlist`**
(`libc/SECCOMP_BLOCKLIST_APP.TXT`, `SECCOMP_ALLOWLIST_COMMON.TXT`).  A number
outside that final allowlist is killed with `SIGSYS` **before the kernel runs
it** — it is not an `ENOSYS` from the kernel.

The tracer runs in the same app, under the same filter, so it **cannot re-issue
a killed call**.  It can only:

- answer the call itself (a fake, a stateless value);
- substitute an **allowed** equivalent (`accept` -> `accept4`, `faccessat2` ->
  `faccessat`);
- emulate the effect in user space (a broker), or refuse.

That is the constraint that makes the `killed` cases different from all others.

## The kernel gate

If seccomp lets the call through (or the tracer substitutes one), the kernel
decides.  It can lack the call for two reasons, with different costs:

- **version**: the host kernel is older than the call (`clone3` on 4.14);
  substitute the older call or answer `ENOSYS`.
- **config**: the kernel was built without it.  The GKI `android-base.config`
  has `# CONFIG_SYSVIPC is not set` on 5.10, 5.15 and 6.1; the S10e confirms it
  (no `/proc/sysvipc`, no `/proc/sys/kernel/shmmax`).  A broker in user space is
  the only way to provide it.

## The four cases

| seccomp | kernel | example | answer |
| --- | --- | --- | --- |
| allowed | native | `read`, `mmap`, `unshare`* | pass |
| allowed | version/config | `clone3` on 4.14, `fchmodat2` | substitute, else `ENOSYS` |
| killed | native | `setuid`, `accept`, `faccessat2` | fake / substitute an allowed call |
| killed | version/config | SysV, `mq_*`, `io_uring`, keyrings | emulate (broker) or refuse |

\* `unshare`/`setns` are in `SYSCALLS.TXT` (seccomp allows them); SELinux (and,
for the privileged namespaces, the missing `CAP_SYS_ADMIN`) is what refuses
them.  Namespaces are not seccomp-killed.

## Findings

Measured/checked 2026-10-18 against the bionic sources (the `android12-release`
branch for the S10e, `main` for Android 16), the GKI config fragments and the
census:

| call | seccomp | kernel | note |
| --- | --- | --- | --- |
| SysV `shm*`/`sem*`/`msg*` | killed (not in the allowlist) | config off | double-blocked; a broker must use `memfd`/files/unix sockets, not `shmget` |
| POSIX `mq_*` | killed | config off | double-blocked; rare |
| `io_uring_*` | killed | version on 4.14; present on 5.10 | policy on both phones |
| `keyctl`/`add_key`/`request_key` | killed | `ENOSYS` | double-blocked |
| `finit_module`, `lookup_dcookie` | killed | native | the runtime answers `EPERM` |
| `bpf` | killed on Android 12; allowed on 16 | Android 16: `EPERM` (no `CAP_BPF`) | same answer, different gate |
| `clone3`, `faccessat2`, `openat2`, `fchmodat2`, `epoll_pwait2` | killed on Android 12; allowed on 16 (census) | version on 4.14 | `clone3` is in the Android 16 allowlist |
| `perf_event_open`, `userfaultfd`, `seccomp` | allowed (allowlist) | the kernel's own policy (`EACCES`/`EPERM`) | not a seccomp denial |
| `unshare`, `setns` | allowed | SELinux/capabilities | enforcement, not a kill |
| `acct`, `__reboot` | killed (blocklist) | native | skipped by the census (global state); the runtime currently answers `ENOSYS`, not `EPERM` |

## Caveats

- AOSP is the **baseline** intent; a vendor kernel and its policy can differ.
  The S10e runs a Samsung 4.14 kernel; the S23 FE runs a vendor 5.10.  The
  device census is the per-device truth; this page explains *why*.
- The bionic files move between releases (`android12-release` vs `main`); check
  the branch for the phone, not just `main`.
- The `run-as` domain has `Seccomp: 0` but is `runas_app`, not the app domain;
  it is a probe for the kernel side, not proof about SELinux.

## Sources

- bionic, `libc/SYSCALLS.TXT`, `SECCOMP_BLOCKLIST_APP.TXT`,
  `SECCOMP_ALLOWLIST_COMMON.TXT`, `SECCOMP_PRIORITY.TXT`:
  <https://android.googlesource.com/platform/bionic/>
- GKI, `kernel/configs/android-base.config` (Android 12/13/14 branches):
  <https://android.googlesource.com/kernel/common/>
- the device measurements: [`device-census.md`](device-census.md).
