# Syscall classes: transform, placement, validation

A per-syscall map of what the runtime does, why, and where -- the basis for
deciding what belongs in the fast path and what must stay in the net.  The
counts come from [`device-census.md`](device-census.md) (S10e, Android 12,
kernel 4.14, taken 2026-10-13/16; S23 FE, Android 16, kernel 5.10) and were
checked against `src/dn-tiny/` on 2026-10-17.

## The three axes

Every syscall is described by three independent questions.  The first is
*semantics*, the second is *speed*, the third is *why the answer is right*; they
must not be conflated (a "fast path for Android" and a "fast path for the
illusion" are the same mechanism -- see the note at the end).

1. **Transform** -- what the program is told:
   - `pass`: run it, return the kernel's result (the kernel is the truth);
   - `map`: run it on a host path/coordinate (the law);
   - `emulate`: answer or rewrite it (identity, `/proc` files, substitutes);
   - `substitute`: run an equivalent syscall (`accept` -> `accept4`);
   - `refuse`: answer a truthful `EPERM`/`ENOSYS` (never invent success).
2. **Placement** -- where the work happens:
   - `kernel` (no intervention);
   - `shim symbol` (in process, `dn-lane.so` interposes a libc symbol);
   - `lane` (the shim's own call carries the cookie, no stop);
   - `site patch` (in process, a libc/loader `svc` site is redirected);
   - `tracer entry` (the tracer maps/answers at the entry stop);
   - `tracer SIGSYS` (the platform killed the call; `trace_denied` answers);
   - `exec gate` (the tracer rewrites `execve`).
3. **Validation** -- why the answer is right:
   - `law`: the one path/identity law (`policy.c`);
   - `state`: per-process state (ids, cwd, exe);
   - `explicit`: a named list (`policy_deny`'s `EPERM` cases, `D_SUBST`);
   - `default`: any killed number not otherwise known gets `ENOSYS`;
   - `kernel`: the kernel itself decides (pass-through);
   - `fail-safe patch`: a wrong patch costs a stop, not a wrong answer;
   - `cache + invalidation`: a cheap oracle (generation, ctime) says when a
     cached answer is stale.

## The device matrix

| | Android 12, 4.14 | Android 16, 5.10 |
| --- | --- | --- |
| allowed in the app | 201 | 217 |
| killed with `SIGSYS` | 256 | 240 |
| of those, the kernel implements the call | **32** | **42** |
| of those, the kernel lacks the call | 224 | 198 |

The 32 and 42 are the *real denials*; the 224/198 are answered `ENOSYS` by
default (they are calls this kernel does not offer).

## S10e: the 32 real denials

All of them arrive at a **`SIGSYS` stop** (Android's `TRAP` outranks the
runtime's `TRACE`), so `placement` is `tracer SIGSYS` for the whole table
unless noted.  **None has a fast path today, and none needs one**: each is
answered once per call, not in a hot loop.

| group | calls | transform | validation | notes |
| --- | --- | --- | --- | --- |
| 1 | `accept` | `substitute` -> `accept4` | explicit (`D_SUBST`; flags checked) | the re-issued `accept4` then maps its AF_UNIX address through row 242 |
| 8 | `setuid` `setgid` `setreuid` `setregid` `setresgid` `setfsuid` `setfsgid` `setgroups` | `emulate` (remembered) | state per process | row `FAKE`; `setresuid` (not killed, kernel says `EPERM`) has the same transform at the entry stop |
| 17 | `mount` `umount2` `chroot` `swapon` `swapoff` `clock_settime` `settimeofday` `adjtimex` `clock_adjtime` `sethostname` `setdomainname` `syslog` `init_module` `delete_module` `finit_module` `lookup_dcookie` `bpf` | `refuse` (`EPERM`) | explicit | |
| 6 | `set_robust_list` `get_robust_list` `remap_file_pages` `add_key` `request_key` `keyctl` | `refuse` (`ENOSYS`) | default | |

`getuid`/`geteuid`/`getgid`/`getegid`/`getresuid`/`getresgid` are **not**
denials; they are allowed and emulated in process (see the allowed table).
`set*id` have **no** shim fast path: they are state, handled by the tracer.

## S10e: the 224 kernel-lacks

| group | examples | transform | placement | validation |
| --- | --- | --- | --- | --- |
| 224 | `rseq` `clone3` `close_range` `openat2` `faccessat2` `fchmodat2` `epoll_pwait2` `io_uring_*` `futex_waitv` `landlock_*`, SysV/POSIX mqueue, `mbind*`, `kcmp`, `*_handle_at`, unassigned numbers | `refuse` (`ENOSYS`) | tracer SIGSYS | default |

SysV IPC is here, not in the 32: Android's kernel configuration lacks it, so
`run-as` says `ENOSYS` too.  When it is emulated (TODO, "Emulation"), it becomes
a new transform (`emulate`, broker in the tracer + `shmat` in the shim) with
`state` validation; it still will not be a fast path at first.

## Android 16: 42 = 32 - `bpf` + 11

| change | calls | transform | placement | validation |
| --- | --- | --- | --- | --- |
| +11 | `io_uring_setup` `io_uring_enter` `io_uring_register`, `open_tree` `move_mount` `fsopen` `fsconfig` `fsmount` `fspick`, `kcmp`, `io_pgetevents` | `refuse` (`ENOSYS`) | tracer SIGSYS | default |
| -1 | `bpf` | `pass` (the kernel says `EPERM` to an unprivileged caller) | kernel | kernel |
| newly allowed | `faccessat2` `openat2` `clone3` `close_range` `fchmodat2` `epoll_pwait2` | see below | see below | see below |

No new row is needed for the 11: `policy_deny` answers a killed number in no
row with `ENOSYS`.  `tests/bench/syscalls.c` runs exactly this list.

On 5.10 the six newly-allowed calls split:
- `clone3`, `close_range`: `pass` -- no row, the kernel implements them.
- `epoll_pwait2`: `pass` -- no row, the kernel says `ENOSYS` (5.10 lacks it).
- `openat2` (437), `faccessat2` (439): `map` -- a row maps the path at the entry
  stop; the kernel then runs them natively.
- `fchmodat2` (452): `map` + kernel `ENOSYS` -- a row maps the path, the kernel
  lacks the call, and the caller sees `ENOSYS` (glibc tolerates it; the older
  `fchmodat`, row 53, is the fallback when the caller takes it).

So "allowed" does not mean "kernel only": it means the platform does not kill
it; the runtime may still map its path.

## The 201/217 allowed calls: where the fast path lives

| transform | examples | placement | validation | fast path today |
| --- | --- | --- | --- | --- |
| `map` | `openat` `newfstatat` `statx` `readlinkat` `faccessat` `chdir` `mkdirat` ..., exec gate, AF_UNIX `sun_path` | shim symbol / lane, tracer entry (AF_UNIX, exec), loader hook | law | **yes** for the shim's list; **no** for AF_UNIX and exec (tracer only) |
| `emulate` | `get*id`, `getres*id`, `getgroups`/`setgroups` (the virtual list); `/proc` synth (`status`, `cmdline`, `comm`, `cgroup`, `maps`, `smaps`, `uptime`, `loadavg`, `stat`, `vmstat`, `mounts`, `mountinfo`, `/etc/mtab`); `user.dn.owner` | shim symbol (ids), tracer entry/open (synth), xattr read (owner) | state / regenerated at open / xattr | **yes** for ids and synth reads; owner goes through a `(dev, ino, ctime)` cache (first stat per file reads the xattr) |
| `substitute` | `faccessat2`->`faccessat`, `fchmodat2`->`fchmodat`, `openat2`->`openat` (only when killed; `RESOLVE_*` -> `ENOSYS`) | tracer SIGSYS | explicit | no (not a hot path) |
| `emulate` (identity) | `openat` write as fake root (temporary owner-write, restored), `prctl(PR_GET_NO_NEW_PRIVS)`/`PR_SET_DUMPABLE`, the setuid-bit exec gate, `faccessat`/`access` from the virtual ids (root's execute-bit rule; owner, group, other) | tracer entry / exit, exec gate; the shim answers `access` when stat decides it | answer + restore / stored owner / the group list | only the fake root's write open costs a chmod; `access` is a local stat, no stop, unless a supplementary group decides |
| `pass` | `read` `write` `close` `mmap` `futex` `epoll` `signals`, and sockets without an AF_UNIX path ... | kernel | kernel | yes (by definition) |

Also allowed and transformed, but not in the tables above: `accept4` (row 242
maps its AF_UNIX address; `accept` reaches it through the `D_SUBST` above),
`linkat`/`unlinkat` (tracer, for the copy counts; the shim only adjusts the
cache after the real call), `statfs`/`truncate`/path-only xattr/mode calls
(the shim's lane list).

## What the classification says

- **Denials are policy, not performance.**  The 32/42 are one `SIGSYS` stop
  each, answered by an explicit list or the default; building a fast path there
  would be wasted.
- **The illusion's cost is in the allowed-but-transformed set** -- exactly where
  the shim, the lane, the loader hook and the caches live.  The table names the
  gaps: AF_UNIX, the `openat2`/`faccessat2` rows, `syscall()` generic, `dlopen`ed
  libraries, static/Go/musl.
- **One mechanism, two labels.**  A denial (`SIGSYS` + policy) and an allowed
  mapping (`TRACE` + row + law) are the same kind of work -- transform,
  placement, validation.  "Fast path for the illusion" and "fast path for
  Android" are not two systems; a fast path only chooses a cheaper placement for
  the same transform, and must be fail-safe.
- **The default is the shield.**  A killed number with no row is `ENOSYS`; an
  allowed number with no transform is the kernel's.  New kernels (the 11 on
  Android 16) need no new row for denials.
- **`pass` needs no work, `refuse` gets no fast path.**  The only interesting
  work is `map`/`emulate` on hot allowed calls.

## Fact-check notes (2026-10-17)

Checked against the code and the census tables:

- The 8 + 1 + 17 + 6 and the call names match `device-census.md`; the answers
  match `policy_deny` (explicit `EPERM`/`D_SUBST` cases, `ENOSYS` default) and
  the `FAKE` rows in `gen_rows.py`.
- Only `getuid`/`geteuid`/`getgid`/`getegid`/`getresuid`/`getresgid` are shim
  symbols; `set*id` are tracer state, so "no fast path" is accurate.
- AF_UNIX calls (`bind`/`connect`/`send*`/`recv*`/`accept*`) have no shim
  symbol; the tracer's `SOCK` rows map the `sun_path`.  No in-process fast path.
- Rows 437 (`openat2`), 439 (`faccessat2`) and 452 (`fchmodat2`) exist, so the
  Android-16 split is "tracer maps, kernel runs/refuses", not "kernel only".
- SysV is in the 224 (kernel lacks it), not in the 32; the 62-number sample in
  the census predates the full-range counts -- the full-range table is the
  authoritative one.
- `bpf` moves from the 17 `EPERM` cases (4.14) to `pass` on 5.10 (the kernel
  refuses it unprivileged), which is why 42 = 32 - 1 + 11.
- The owner store (`user.dn.owner`) is cached by `(dev, ino, ctime)` in both the
  shim and the tracer (done 2026-10-17); only the first `stat` of a file reads
  the xattr.  A chown is stored even for 0:0, so an explicit chown reads the same
  in the fake world and for the real user; a file that was never chowned falls
  back to the real uid (the fake world maps it to 0, the real user leaves it).
  A create is marked with the creator's ids (`openat`+`O_CREAT`/`mkdirat`/
  `mknodat`; the shim marks `mkdirat`/`mknodat` on the lane), so it no longer
  depends on the reader's world.  A shared store-used flag is still open.

## Caveats

- Counts are per device and kernel; they are not a survey of the whole Debian
  archive.  The allowed/denied split changes with the platform's seccomp policy.
- The census covers numbers 0..470 (14 skipped); a syscall above that range is
  outside it.
- `pass` on one device can be `refuse` on another (`bpf`, `openat2`); the table
  says so where it matters.
