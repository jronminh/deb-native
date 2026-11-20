# Feature inventory

Current truth (2026-10-19): what the runtime does, and where each feature lives.
This is the basis for the restructure proposed in `docs/spec/runtime-revision.md`.
Column **kind**: C = correctness (load-bearing), S = speed (fast path, optional),
K = context/bootstrap (should be handed down, not re-derived).

Component legend: `law` = `policy.c` (shared by all engines), `T` = `dn-tiny`
(`monitor.c`/`trace.c`/`exec.c`), `S` = `dn-lane.so` (`lane.c`), `L` =
`dn-ldhook.so` (`ldhook.c`), `boot` = `boot.sh`/`init.sh`, `build` = `scripts/`,
`X` = `dn-exec.c` (POC, `dev/loader`).

## The law (path model) -- `policy.c`

| feature | where | kind |
| --- | --- | --- |
| guest<->host mapping table (longest prefix) | law | C |
| symlink walk in the guest frame (`..`, absolute targets are the tree's) | law | C |
| binds (`boot/binds`) as rows; mount points; `EXDEV`; `ro` -> `EROFS` | law | C |
| dirfd resolution; host directories stay | law | C |
| readlink answer cache (generation `dn-tiny.gen`, seqlock) | law | C |
| `policy_preload`/`policy_ldhook` (shim/ldhook host paths) | law | K |

## Tracer -- catch and filter (`monitor.c`)

| feature | where | kind |
| --- | --- | --- |
| seccomp filter from the rows (JGE tree, lane leaf) | T + gen_rows.py | C |
| ptrace wait loop; read/write registers and memory | T | C |
| lane: cookie in x5 -> read-only calls pass without a stop | T + lane.h | S |
| entry/exit stops; answer-at-entry; exit results | T | C |
| `-s` poll, `-m` modes, `-c` pin, summary | T | K |

## Exec gate -- `exec.c`

| feature | where | kind |
| --- | --- | --- |
| catch `execve`(221) / `execveat`(281) | T | C |
| shebang resolution (script -> interpreter, recursive) | T | C |
| read `PT_INTERP`; run the loader with `--preload/--audit/--argv0` | T | C |
| static/foreign pass-through | T | C |
| `execveat` `AT_EMPTY_PATH` / dirfd-relative | T | C |
| setuid identity gate; inject `DN_IDS`/`DN_REAL_UID`/`DN_REAL_GID` | T | C |
| synthesize `cmdline`/`comm` (loader / `dn-exec` tokens) | T | C |

## Tracer -- apply (`trace.c`)

| feature | where | kind |
| --- | --- | --- |
| path in: map guest->host at entry (openat, fstatat, readlinkat, mknodat, mkdirat, unlinkat, symlinkat, linkat, renameat, statfs, truncate, faccessat, chdir, chroot, fchmodat, utimensat, inotify_add_watch, openat2, faccessat2, fchmodat2) | T | C |
| path out: getcwd/readlink/`/proc/pid/fd` map host->guest | T | C |
| answer-at-entry: readlinkat, fstatat, statx, identity | T | C |
| stat owner + symlink size | T | C |
| fake-root DAC: add owner-write bit, restore at exit | T | C |
| hard link -> copy; `st_nlink` groups; unlink-copy | T | C |
| owner file (`user.dn.owner`) store/read/apply | T | C |
| `/proc` filtering (getdents64 -> the tree's pids) | T | C |
| AF_UNIX: `sun_path` map in/out; abstract names (ADB); placeholder + `S_IFSOCK` | T | C |
| `/dev/shm` -> `<root>/run/shm` | law | C |
| `sched_getaffinity` (stable nproc/taskset) | T | C |
| denied syscalls (SIGSYS): ENOSYS / substitute | law + T | C |

## `/proc` and disguise

| feature | where | kind |
| --- | --- | --- |
| `/proc/<pid>/status`: Uid/Gid/TracerPid/Seccomp/Name | T | C |
| `/proc/<pid>/cmdline`, `comm`, `maps`/`smaps` | T | C |
| `/proc/stat`, uptime, loadavg, cgroup, vmstat | T | C |
| mount view: mounts/mountinfo/mtab from the table | law + T | C |
| `/proc/self/exe` -> the program (exe_now) | T | C |

## Identity -- `trace.c` + `lane.c`

| feature | where | kind |
| --- | --- | --- |
| fake root: one real uid, virtual principals; per-pid state by full pid | T | C |
| set*id/get*id/getgroups/setgroups | T | C |
| `access`/`faccessat` world view | T + S | C |
| chown stored; creates marked with the creator's ids | T | C |
| setuid-bit exec -> euid 0; sudo/su/passwd | T | C |
| `NoNewPrivs`=0; `PR_SET_DUMPABLE` ignored; capget/capset | T | C |

## Shim fast path -- `lane.c`

| feature | where | kind |
| --- | --- | --- |
| interpose symbols: open/openat/stat/lstat/fstatat/fstat/statx/access/faccessat/readlink(at)/realpath/getcwd/get*id/set*id/link/unlink/getxattr... | S | S |
| patch libc `svc` sites; cancellable `svc` (`DN_CANCEL_OFF`) | S | S |
| lane cookie on its own calls | S | S |
| discover root (dladdr -> maps); parse `boot/binds`; mmap `dn-tiny.gen` | S | K |
| identity fast path (`DN_IDS` hint, access world) | S | S |
| hard-link copy/link count in-process (seqlock, per-pid) | S | S |
| thread/vfork safety (seqlock, atfork) | S | C |

## Loader hook -- `ldhook.c`

| feature | where | kind |
| --- | --- | --- |
| patch the loader's `openat`/`faccessat` -> law + lane | L | S |
| locate the loader (maps or `AT_BASE`); root from its own path | L | K |

## Boot glue -- `boot.sh` / `init.sh`

| feature | where | kind |
| --- | --- | --- |
| boot.sh: root, clean env, identity, exec dn-tiny | boot | K |
| init.sh: locale, rseq off, `GLIBC_TUNABLES`, `/etc/passwd` reconcile, setuid, `/run/shm`, resolv.conf, policy-rc.d | boot | K |

## Build -- `scripts/`

| feature | where | kind |
| --- | --- | --- |
| build-runtime.sh: dn-tiny + dn-ldhook (+ dn-exec) | build | K |
| build-prefix.sh: debootstrap + build the shim in the tree + apt | build | K |
| build-busybox.sh, adb deploy/shell | build | K |

## `dn-exec` (POC, `dev/loader`)

| feature | where | kind |
| --- | --- | --- |
| userland exec: map program + loader, build stack/auxv, jump | X | C |
| guest `LD_PRELOAD`/`LD_AUDIT`, `--root`, lane cookie | X | K |

## Denied syscalls (`policy_deny`, answered at SIGSYS)

| syscall | answer |
| --- | --- |
| setregid/setgid/setreuid/setuid/setresgid/setfsuid/setfsgid/setgroups | 0 (as root would) |
| accept | accept4 (substitute) |
| lookup_dcookie/umount2/mount/chroot/init_module/delete_module/finit_module/clock_settime/settimeofday/adjtimex/clock_adjtime/syslog/sethostname/setdomainname/swapon/swapoff/bpf | EPERM |
| faccessat2 | faccessat (substitute) |
| fchmodat2 | fchmodat (substitute) |
| openat2 | openat (substitute) |
| everything else denied (rseq, set_robust_list, clone3, ...) | ENOSYS |

Note: `clone3` is **not** substituted with `clone` here; it is answered `ENOSYS`
and glibc falls back to `clone` itself -- on the SIGSYS slow path.
