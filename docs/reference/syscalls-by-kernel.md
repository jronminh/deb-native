# The arm64 syscall set across kernels

The policy ([`policy-audit.md`](policy-audit.md)) keys on syscall numbers. arm64
uses one generic table, so a number means the same syscall on every arm64 kernel,
and the kernels only **add** (no number is reused). This is the basis for
deciding, per syscall, whether the device runs it natively, returns `ENOSYS`, or
the runtime substitutes one.

## Method (reproducible)

- Fetch `include/uapi/asm-generic/unistd.h` for a tag from the kernel git
  (`https://raw.githubusercontent.com/torvalds/linux/<tag>/...`).
- Take each `#define __NR_name N` with a plain number. **Skip** the
  `__NR_syscalls` sentinel and the `nr >= 1024` block (the 32-bit compat legacy
  numbers): both are header artifacts, not arm64 syscalls, and including them
  invents changes that did not happen.
- The Debian reference is the `linux-libc-dev` header a prefix ships
  (`usr/include/asm-generic/unistd.h`), which is the Debian arm64 kernel's; on
  this prefix it is byte-identical to mainline 6.12.

## Count

| kernel | real syscalls |
| --- | --- |
| 4.14 (S10e) | 266 |
| 5.10 (S23 FE) | 306 |
| 6.1 | 316 |
| 6.6 | 318 |
| 6.12 = Debian 13 | 328 |

## Added, between the tags

- **4.14 -> 5.10 (+40):** `io_pgetevents`, `rseq`, `kexec_file_load`, the
  `*_time64` family, `pidfd_send_signal`/`pidfd_open`/`pidfd_getfd`, `io_uring_*`,
  `open_tree`/`move_mount`/`fsopen`/`fsconfig`/`fsmount`/`fspick`, `clone3`,
  `close_range`, `openat2`, `faccessat2`, `process_madvise`.
- **5.10 -> 6.1 (+10):** `epoll_pwait2`, `mount_setattr`, `quotactl_fd`, the
  `landlock_*` calls, `memfd_secret`, `process_mrelease`, `futex_waitv`,
  `set_mempolicy_home_node`.
- **6.1 -> 6.6 (+2):** `cachestat`, `fchmodat2`.
- **6.6 -> 6.12 (+10):** `map_shadow_stack`, `futex_wake`/`wait`/`requeue`,
  `statmount`, `listmount`, the `lsm_*` calls, `mseal`.

## What Debian has and the S10e (4.14) lacks: 62

`io_pgetevents`, `rseq`, `kexec_file_load`, `clock_gettime64`, `clock_settime64`,
`clock_adjtime64`, `clock_getres_time64`, `clock_nanosleep_time64`,
`timer_gettime64`, `timer_settime64`, `timerfd_gettime64`, `timerfd_settime64`,
`utimensat_time64`, `pselect6_time64`, `ppoll_time64`, `io_pgetevents_time64`,
`recvmmsg_time64`, `mq_timedsend_time64`, `mq_timedreceive_time64`,
`semtimedop_time64`, `rt_sigtimedwait_time64`, `futex_time64`,
`sched_rr_get_interval_time64`, `pidfd_send_signal`, `io_uring_setup`,
`io_uring_enter`, `io_uring_register`, `open_tree`, `move_mount`, `fsopen`,
`fsconfig`, `fsmount`, `fspick`, `pidfd_open`, `clone3`, `close_range`,
`openat2`, `pidfd_getfd`, `faccessat2`, `process_madvise`, `epoll_pwait2`,
`mount_setattr`, `quotactl_fd`, `landlock_create_ruleset`, `landlock_add_rule`,
`landlock_restrict_self`, `memfd_secret`, `process_mrelease`, `futex_waitv`,
`set_mempolicy_home_node`, `cachestat`, `fchmodat2`, `map_shadow_stack`,
`futex_wake`, `futex_wait`, `futex_requeue`, `statmount`, `listmount`,
`lsm_get_self_attr`, `lsm_set_self_attr`, `lsm_list_modules`, `mseal`.

For each, the S10e returns `ENOSYS`, so glibc falls back (it does for most), or
the policy substitutes (`faccessat2` -> `faccessat`, `openat2` -> `openat`, ...).
The S23 FE (5.10) has the first 40 and lacks the rest.

## What it means for the policy

- The numbers are stable, so a row keyed by number is valid on every device; a
  new syscall is a new number to add a row (or a deny answer) for, never a
  renumbering.
- The list above is the exact set the older device lacks, to check the policy's
  deny/substitute answers against.
