# Two gates: seccomp and the kernel

A syscall can be refused by the Android app filter or by the kernel that
would serve it. deb-native chooses a pass-through, substitution, emulation
or refusal according to those outcomes. [FOA platform gates](https://github.com/jronminh/fhs-on-android/blob/main/reference/problem.md)
and [syscall reference](https://github.com/jronminh/fhs-on-android/blob/main/reference/syscalls.md)

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
