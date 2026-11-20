# Android 16 syscall sweep

**Measured 2026-10-07** on one Samsung Galaxy S23 FE (SM-S7110, arm64),
Android 16, kernel 5.10.240. The probe ran in `untrusted_app_27` (target
API 28, `Seccomp: 2`) and in the Android shell domain (`Seccomp: 0`)
on the same kernel, with SELinux enforcing in both.

The probe isolated each syscall in a separate child, passed zero arguments,
and used a one-second timer. `SIGSYS` records a seccomp trap. Other outcomes
record what happened with those arguments. A shell result of `ENOSYS` gives
the probe's operational classification of an unavailable call. The model
column records whether the allowlist reconstructed from bionic's
`android16-qpr2-release` source included the named syscall.

## Results

- Range 0–480 contains 481 slots, 318 named in the source table and 163
  unnamed. The three skipped named slots (`exit`, `exit_group` and `seccomp`)
  leave 478 attempted slots.
- The app trapped on 253 attempted slots, 94 named and 159 unnamed. The
  shell trapped on none. Of the 94 named traps, 44 reached an implementation
  in the shell and 50 returned `ENOSYS`.
- All comparable named rows match the bionic model except `process_mrelease`
  (448). The source model excludes it, while the app probe reached it and
  got `EBADF`. No cause was established for the discrepancy.
- Unnamed slots 244–259, 295–423 and 467–480 trapped in the app and returned
  `ENOSYS` in the shell. Slots 463–466 returned `ENOSYS` in both.

FOA's later Android 16 census counts 42 kernel-present denials using
0–470 with 14 exclusions and a different control. Compare 42 with this
sweep's 44 kernel-present traps. For example, the later probe skips
`vhangup` and `reboot`, both of which count in the earlier 44.
The 94 total also includes 50 named
calls that were unavailable in the shell control.
[FOA syscall reference](https://github.com/jronminh/fhs-on-android/blob/main/reference/syscalls.md) ·
[deb-native census](../docs/reference/device-census.md)

## Named syscalls

| nr | name | app | shell | model allows |
|---:|---|---|---|---|
| 0 | `io_setup` | EFAULT | EFAULT | yes |
| 1 | `io_destroy` | EINVAL | EINVAL | yes |
| 2 | `io_submit` | EINVAL | EINVAL | yes |
| 3 | `io_cancel` | EFAULT | EFAULT | yes |
| 4 | `io_getevents` | EINVAL | EINVAL | yes |
| 5 | `setxattr` | EFAULT | EFAULT | yes |
| 6 | `lsetxattr` | EFAULT | EFAULT | yes |
| 7 | `fsetxattr` | EFAULT | EFAULT | yes |
| 8 | `getxattr` | EFAULT | EFAULT | yes |
| 9 | `lgetxattr` | EFAULT | EFAULT | yes |
| 10 | `fgetxattr` | EFAULT | EFAULT | yes |
| 11 | `listxattr` | EFAULT | EFAULT | yes |
| 12 | `llistxattr` | EFAULT | EFAULT | yes |
| 13 | `flistxattr` | ok | ok | yes |
| 14 | `removexattr` | EFAULT | EFAULT | yes |
| 15 | `lremovexattr` | EFAULT | EFAULT | yes |
| 16 | `fremovexattr` | EFAULT | EFAULT | yes |
| 17 | `getcwd` | ERANGE | ERANGE | yes |
| 18 | `lookup_dcookie` | **SIGSYS** | EPERM | no |
| 19 | `eventfd2` | ok | ok | yes |
| 20 | `epoll_create1` | ok | ok | yes |
| 21 | `epoll_ctl` | EFAULT | EFAULT | yes |
| 22 | `epoll_pwait` | EINVAL | EINVAL | yes |
| 23 | `dup` | ok | ok | yes |
| 24 | `dup3` | EINVAL | EINVAL | yes |
| 25 | `fcntl` | ok | ok | yes |
| 26 | `inotify_init1` | ok | ok | yes |
| 27 | `inotify_add_watch` | EINVAL | EINVAL | yes |
| 28 | `inotify_rm_watch` | EINVAL | EINVAL | yes |
| 29 | `ioctl` | EACCES | EACCES | yes |
| 30 | `ioprio_set` | EINVAL | EINVAL | yes |
| 31 | `ioprio_get` | EINVAL | EINVAL | yes |
| 32 | `flock` | EINVAL | EINVAL | yes |
| 33 | `mknodat` | EFAULT | EFAULT | yes |
| 34 | `mkdirat` | EFAULT | EFAULT | yes |
| 35 | `unlinkat` | EFAULT | EFAULT | yes |
| 36 | `symlinkat` | EFAULT | EFAULT | yes |
| 37 | `linkat` | EFAULT | EFAULT | yes |
| 38 | `renameat` | EFAULT | EFAULT | yes |
| 39 | `umount2` | **SIGSYS** | EFAULT | no |
| 40 | `mount` | **SIGSYS** | EFAULT | no |
| 41 | `pivot_root` | EPERM | EPERM | yes |
| 42 | `nfsservctl` | **SIGSYS** | ENOSYS | no |
| 43 | `statfs` | EFAULT | EFAULT | yes |
| 44 | `fstatfs` | EFAULT | EFAULT | yes |
| 45 | `truncate` | EFAULT | EFAULT | yes |
| 46 | `ftruncate` | EINVAL | EINVAL | yes |
| 47 | `fallocate` | EINVAL | EINVAL | yes |
| 48 | `faccessat` | EFAULT | EFAULT | yes |
| 49 | `chdir` | EFAULT | EFAULT | yes |
| 50 | `fchdir` | errno 20 | errno 20 | yes |
| 51 | `chroot` | **SIGSYS** | EFAULT | no |
| 52 | `fchmod` | EACCES | EACCES | yes |
| 53 | `fchmodat` | EFAULT | EFAULT | yes |
| 54 | `fchownat` | EFAULT | EFAULT | yes |
| 55 | `fchown` | EACCES | EACCES | yes |
| 56 | `openat` | EFAULT | EFAULT | yes |
| 57 | `close` | ok | ok | yes |
| 58 | `vhangup` | **SIGSYS** | EPERM | no |
| 59 | `pipe2` | EFAULT | EFAULT | yes |
| 60 | `quotactl` | ENODEV | ENODEV | yes |
| 61 | `getdents64` | errno 20 | errno 20 | yes |
| 62 | `lseek` | ESPIPE | ESPIPE | yes |
| 63 | `read` | ok | ok | yes |
| 64 | `write` | EBADF | ok | yes |
| 65 | `readv` | ok | ok | yes |
| 66 | `writev` | EBADF | ok | yes |
| 67 | `pread64` | ESPIPE | ESPIPE | yes |
| 68 | `pwrite64` | ESPIPE | ESPIPE | yes |
| 69 | `preadv` | ESPIPE | ESPIPE | yes |
| 70 | `pwritev` | ESPIPE | ESPIPE | yes |
| 71 | `sendfile` | EBADF | EINVAL | yes |
| 72 | `pselect6` | blocks (timer) | blocks (timer) | yes |
| 73 | `ppoll` | blocks (timer) | blocks (timer) | yes |
| 74 | `signalfd4` | EINVAL | EINVAL | yes |
| 75 | `vmsplice` | ok | ok | yes |
| 76 | `splice` | ok | ok | yes |
| 77 | `tee` | ok | ok | yes |
| 78 | `readlinkat` | EINVAL | EINVAL | yes |
| 79 | `newfstatat` | EFAULT | EFAULT | yes |
| 80 | `fstat` | EFAULT | EFAULT | yes |
| 81 | `sync` | ok | ok | yes |
| 82 | `fsync` | EINVAL | EINVAL | yes |
| 83 | `fdatasync` | EINVAL | EINVAL | yes |
| 84 | `sync_file_range` | ESPIPE | ESPIPE | yes |
| 85 | `timerfd_create` | ok | ok | yes |
| 86 | `timerfd_settime` | EFAULT | EFAULT | yes |
| 87 | `timerfd_gettime` | EINVAL | EINVAL | yes |
| 88 | `utimensat` | ok | ok | yes |
| 89 | `acct` | **SIGSYS** | ENOSYS | no |
| 90 | `capget` | EFAULT | EFAULT | yes |
| 91 | `capset` | EFAULT | EFAULT | yes |
| 92 | `personality` | ok | ok | yes |
| 93 | `exit` | (skipped) | (skipped) | yes |
| 94 | `exit_group` | (skipped) | (skipped) | yes |
| 95 | `waitid` | EINVAL | EINVAL | yes |
| 96 | `set_tid_address` | ok | ok | yes |
| 97 | `unshare` | ok | ok | yes |
| 98 | `futex` | EFAULT | EFAULT | yes |
| 99 | `set_robust_list` | **SIGSYS** | EINVAL | no |
| 100 | `get_robust_list` | **SIGSYS** | EFAULT | no |
| 101 | `nanosleep` | EFAULT | EFAULT | yes |
| 102 | `getitimer` | EFAULT | EFAULT | yes |
| 103 | `setitimer` | ok | ok | yes |
| 104 | `kexec_load` | **SIGSYS** | ENOSYS | no |
| 105 | `init_module` | **SIGSYS** | EPERM | no |
| 106 | `delete_module` | **SIGSYS** | EPERM | no |
| 107 | `timer_create` | EFAULT | EFAULT | yes |
| 108 | `timer_gettime` | EINVAL | EINVAL | yes |
| 109 | `timer_getoverrun` | EINVAL | EINVAL | yes |
| 110 | `timer_settime` | EINVAL | EINVAL | yes |
| 111 | `timer_delete` | EINVAL | EINVAL | yes |
| 112 | `clock_settime` | **SIGSYS** | EFAULT | no |
| 113 | `clock_gettime` | EFAULT | EFAULT | yes |
| 114 | `clock_getres` | ok | ok | yes |
| 115 | `clock_nanosleep` | EFAULT | EFAULT | yes |
| 116 | `syslog` | **SIGSYS** | EPERM | no |
| 117 | `ptrace` | ok | ok | yes |
| 118 | `sched_setparam` | EINVAL | EINVAL | yes |
| 119 | `sched_setscheduler` | EINVAL | EINVAL | yes |
| 120 | `sched_getscheduler` | ok | ok | yes |
| 121 | `sched_getparam` | EINVAL | EINVAL | yes |
| 122 | `sched_setaffinity` | EINVAL | EINVAL | yes |
| 123 | `sched_getaffinity` | EINVAL | EINVAL | yes |
| 124 | `sched_yield` | ok | ok | yes |
| 125 | `sched_get_priority_max` | ok | ok | yes |
| 126 | `sched_get_priority_min` | ok | ok | yes |
| 127 | `sched_rr_get_interval` | EFAULT | EFAULT | yes |
| 128 | `restart_syscall` | errno 4 | errno 4 | yes |
| 129 | `kill` | ok | ok | yes |
| 130 | `tkill` | EINVAL | EINVAL | yes |
| 131 | `tgkill` | EINVAL | EINVAL | yes |
| 132 | `sigaltstack` | ok | ok | yes |
| 133 | `rt_sigsuspend` | EINVAL | EINVAL | yes |
| 134 | `rt_sigaction` | EINVAL | EINVAL | yes |
| 135 | `rt_sigprocmask` | EINVAL | EINVAL | yes |
| 136 | `rt_sigpending` | ok | ok | yes |
| 137 | `rt_sigtimedwait` | EINVAL | EINVAL | yes |
| 138 | `rt_sigqueueinfo` | EFAULT | EFAULT | yes |
| 139 | `rt_sigreturn` | SIGSEGV | SIGSEGV | yes |
| 140 | `setpriority` | ok | ok | yes |
| 141 | `getpriority` | ok | ok | yes |
| 142 | `reboot` | **SIGSYS** | EPERM | no |
| 143 | `setregid` | **SIGSYS** | EPERM | no |
| 144 | `setgid` | **SIGSYS** | EPERM | no |
| 145 | `setreuid` | **SIGSYS** | EPERM | no |
| 146 | `setuid` | **SIGSYS** | EPERM | no |
| 147 | `setresuid` | EPERM | EPERM | yes |
| 148 | `getresuid` | EFAULT | EFAULT | yes |
| 149 | `setresgid` | **SIGSYS** | EPERM | no |
| 150 | `getresgid` | EFAULT | EFAULT | yes |
| 151 | `setfsuid` | **SIGSYS** | ok | no |
| 152 | `setfsgid` | **SIGSYS** | ok | no |
| 153 | `times` | ok | ok | yes |
| 154 | `setpgid` | ok | ok | yes |
| 155 | `getpgid` | ok | ok | yes |
| 156 | `getsid` | ok | ok | yes |
| 157 | `setsid` | ok | ok | yes |
| 158 | `getgroups` | ok | ok | yes |
| 159 | `setgroups` | **SIGSYS** | EPERM | no |
| 160 | `uname` | EFAULT | EFAULT | yes |
| 161 | `sethostname` | **SIGSYS** | EPERM | no |
| 162 | `setdomainname` | **SIGSYS** | EPERM | no |
| 163 | `getrlimit` | EFAULT | EFAULT | yes |
| 164 | `setrlimit` | EFAULT | EFAULT | yes |
| 165 | `getrusage` | EFAULT | EFAULT | yes |
| 166 | `umask` | ok | ok | yes |
| 167 | `prctl` | EINVAL | EINVAL | yes |
| 168 | `getcpu` | ok | ok | yes |
| 169 | `gettimeofday` | ok | ok | yes |
| 170 | `settimeofday` | **SIGSYS** | EPERM | no |
| 171 | `adjtimex` | **SIGSYS** | EFAULT | no |
| 172 | `getpid` | ok | ok | yes |
| 173 | `getppid` | ok | ok | yes |
| 174 | `getuid` | ok | ok | yes |
| 175 | `geteuid` | ok | ok | yes |
| 176 | `getgid` | ok | ok | yes |
| 177 | `getegid` | ok | ok | yes |
| 178 | `gettid` | ok | ok | yes |
| 179 | `sysinfo` | EFAULT | EFAULT | yes |
| 180 | `mq_open` | **SIGSYS** | ENOSYS | no |
| 181 | `mq_unlink` | **SIGSYS** | ENOSYS | no |
| 182 | `mq_timedsend` | **SIGSYS** | ENOSYS | no |
| 183 | `mq_timedreceive` | **SIGSYS** | ENOSYS | no |
| 184 | `mq_notify` | **SIGSYS** | ENOSYS | no |
| 185 | `mq_getsetattr` | **SIGSYS** | ENOSYS | no |
| 186 | `msgget` | **SIGSYS** | ENOSYS | no |
| 187 | `msgctl` | **SIGSYS** | ENOSYS | no |
| 188 | `msgrcv` | **SIGSYS** | ENOSYS | no |
| 189 | `msgsnd` | **SIGSYS** | ENOSYS | no |
| 190 | `semget` | **SIGSYS** | ENOSYS | no |
| 191 | `semctl` | **SIGSYS** | ENOSYS | no |
| 192 | `semtimedop` | **SIGSYS** | ENOSYS | no |
| 193 | `semop` | **SIGSYS** | ENOSYS | no |
| 194 | `shmget` | **SIGSYS** | ENOSYS | no |
| 195 | `shmctl` | **SIGSYS** | ENOSYS | no |
| 196 | `shmat` | **SIGSYS** | ENOSYS | no |
| 197 | `shmdt` | **SIGSYS** | ENOSYS | no |
| 198 | `socket` | EACCES | EACCES | yes |
| 199 | `socketpair` | EFAULT | EFAULT | yes |
| 200 | `bind` | ENOTSOCK | EACCES | yes |
| 201 | `listen` | ENOTSOCK | EACCES | yes |
| 202 | `accept` | **SIGSYS** | EACCES | no |
| 203 | `connect` | ENOTSOCK | EACCES | yes |
| 204 | `getsockname` | ENOTSOCK | EFAULT | yes |
| 205 | `getpeername` | ENOTSOCK | EFAULT | yes |
| 206 | `sendto` | ENOTSOCK | ok | yes |
| 207 | `recvfrom` | ENOTSOCK | ok | yes |
| 208 | `setsockopt` | ENOTSOCK | EACCES | yes |
| 209 | `getsockopt` | ENOTSOCK | EOPNOTSUPP | yes |
| 210 | `shutdown` | ENOTSOCK | ok | yes |
| 211 | `sendmsg` | ENOTSOCK | EFAULT | yes |
| 212 | `recvmsg` | ENOTSOCK | EFAULT | yes |
| 213 | `readahead` | EINVAL | EINVAL | yes |
| 214 | `brk` | ok | ok | yes |
| 215 | `munmap` | EINVAL | EINVAL | yes |
| 216 | `mremap` | EINVAL | EINVAL | yes |
| 217 | `add_key` | **SIGSYS** | EFAULT | no |
| 218 | `request_key` | **SIGSYS** | EFAULT | no |
| 219 | `keyctl` | **SIGSYS** | EINVAL | no |
| 220 | `clone` | ok | ok | yes |
| 221 | `execve` | EFAULT | EFAULT | yes |
| 222 | `mmap` | EINVAL | EACCES | yes |
| 223 | `fadvise64` | ESPIPE | ok | yes |
| 224 | `swapon` | **SIGSYS** | EPERM | no |
| 225 | `swapoff` | **SIGSYS** | EPERM | no |
| 226 | `mprotect` | ok | ok | yes |
| 227 | `msync` | ok | ok | yes |
| 228 | `mlock` | ok | ok | yes |
| 229 | `munlock` | ok | ok | yes |
| 230 | `mlockall` | EINVAL | EINVAL | yes |
| 231 | `munlockall` | ok | ok | yes |
| 232 | `mincore` | ok | ok | yes |
| 233 | `madvise` | ok | ok | yes |
| 234 | `remap_file_pages` | **SIGSYS** | EINVAL | no |
| 235 | `mbind` | **SIGSYS** | ENOSYS | no |
| 236 | `get_mempolicy` | **SIGSYS** | ENOSYS | no |
| 237 | `set_mempolicy` | **SIGSYS** | ENOSYS | no |
| 238 | `migrate_pages` | **SIGSYS** | ENOSYS | no |
| 239 | `move_pages` | **SIGSYS** | ENOSYS | no |
| 240 | `rt_tgsigqueueinfo` | EFAULT | EFAULT | yes |
| 241 | `perf_event_open` | EFAULT | EFAULT | yes |
| 242 | `accept4` | ENOTSOCK | EACCES | yes |
| 243 | `recvmmsg` | ENOTSOCK | ok | yes |
| 260 | `wait4` | errno 10 | errno 10 | yes |
| 261 | `prlimit64` | ok | ok | yes |
| 262 | `fanotify_init` | **SIGSYS** | ENOSYS | no |
| 263 | `fanotify_mark` | **SIGSYS** | ENOSYS | no |
| 264 | `name_to_handle_at` | **SIGSYS** | ENOSYS | no |
| 265 | `open_by_handle_at` | **SIGSYS** | ENOSYS | no |
| 266 | `clock_adjtime` | **SIGSYS** | EFAULT | no |
| 267 | `syncfs` | ok | ok | yes |
| 268 | `setns` | EINVAL | EINVAL | yes |
| 269 | `sendmmsg` | ENOTSOCK | ok | yes |
| 270 | `process_vm_readv` | ok | ok | yes |
| 271 | `process_vm_writev` | ok | ok | yes |
| 272 | `kcmp` | **SIGSYS** | ESRCH | no |
| 273 | `finit_module` | **SIGSYS** | EPERM | no |
| 274 | `sched_setattr` | EINVAL | EINVAL | yes |
| 275 | `sched_getattr` | EINVAL | EINVAL | yes |
| 276 | `renameat2` | EFAULT | EFAULT | yes |
| 277 | `seccomp` | (skipped) | (skipped) | yes |
| 278 | `getrandom` | ok | ok | yes |
| 279 | `memfd_create` | EFAULT | EFAULT | yes |
| 280 | `bpf` | EACCES | EACCES | yes |
| 281 | `execveat` | EFAULT | EFAULT | yes |
| 282 | `userfaultfd` | EPERM | EPERM | yes |
| 283 | `membarrier` | ok | ok | yes |
| 284 | `mlock2` | ok | ok | yes |
| 285 | `copy_file_range` | EINVAL | EINVAL | yes |
| 286 | `preadv2` | ESPIPE | ESPIPE | yes |
| 287 | `pwritev2` | ESPIPE | ESPIPE | yes |
| 288 | `pkey_mprotect` | **SIGSYS** | ENOSYS | no |
| 289 | `pkey_alloc` | **SIGSYS** | ENOSYS | no |
| 290 | `pkey_free` | **SIGSYS** | ENOSYS | no |
| 291 | `statx` | EFAULT | EFAULT | yes |
| 292 | `io_pgetevents` | **SIGSYS** | EINVAL | no |
| 293 | `rseq` | **SIGSYS** | ENOSYS | no |
| 294 | `kexec_file_load` | **SIGSYS** | ENOSYS | no |
| 424 | `pidfd_send_signal` | EBADF | EBADF | yes |
| 425 | `io_uring_setup` | **SIGSYS** | EFAULT | no |
| 426 | `io_uring_enter` | **SIGSYS** | EOPNOTSUPP | no |
| 427 | `io_uring_register` | **SIGSYS** | EOPNOTSUPP | no |
| 428 | `open_tree` | **SIGSYS** | EFAULT | no |
| 429 | `move_mount` | **SIGSYS** | EPERM | no |
| 430 | `fsopen` | **SIGSYS** | EPERM | no |
| 431 | `fsconfig` | **SIGSYS** | EINVAL | no |
| 432 | `fsmount` | **SIGSYS** | EPERM | no |
| 433 | `fspick` | **SIGSYS** | EPERM | no |
| 434 | `pidfd_open` | EINVAL | EINVAL | yes |
| 435 | `clone3` | EINVAL | EINVAL | yes |
| 436 | `close_range` | ok | ok | yes |
| 437 | `openat2` | EINVAL | EINVAL | yes |
| 438 | `pidfd_getfd` | EBADF | EBADF | yes |
| 439 | `faccessat2` | EFAULT | EFAULT | yes |
| 440 | `process_madvise` | EBADF | EBADF | yes |
| 441 | `epoll_pwait2` | ENOSYS | ENOSYS | yes |
| 442 | `mount_setattr` | **SIGSYS** | ENOSYS | no |
| 443 | `quotactl_fd` | **SIGSYS** | ENOSYS | no |
| 444 | `landlock_create_ruleset` | ENOSYS | ENOSYS | yes |
| 445 | `landlock_add_rule` | ENOSYS | ENOSYS | yes |
| 446 | `landlock_restrict_self` | ENOSYS | ENOSYS | yes |
| 447 | `memfd_secret` | **SIGSYS** | ENOSYS | no |
| 448 | `process_mrelease` | EBADF | EBADF | no |
| 449 | `futex_waitv` | **SIGSYS** | ENOSYS | no |
| 450 | `set_mempolicy_home_node` | **SIGSYS** | ENOSYS | no |
| 451 | `cachestat` | **SIGSYS** | ENOSYS | no |
| 452 | `fchmodat2` | ENOSYS | ENOSYS | yes |
| 453 | `map_shadow_stack` | **SIGSYS** | ENOSYS | no |
| 454 | `futex_wake` | **SIGSYS** | ENOSYS | no |
| 455 | `futex_wait` | **SIGSYS** | ENOSYS | no |
| 456 | `futex_requeue` | **SIGSYS** | ENOSYS | no |
| 457 | `statmount` | **SIGSYS** | ENOSYS | no |
| 458 | `listmount` | **SIGSYS** | ENOSYS | no |
| 459 | `lsm_get_self_attr` | **SIGSYS** | ENOSYS | no |
| 460 | `lsm_set_self_attr` | **SIGSYS** | ENOSYS | no |
| 461 | `lsm_list_modules` | **SIGSYS** | ENOSYS | no |
| 462 | `mseal` | ENOSYS | ENOSYS | yes |
