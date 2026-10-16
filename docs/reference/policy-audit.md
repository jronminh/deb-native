# Policy audit

The policy is two places: `src/dn-tiny/gen_rows.py` (the rows: which syscall is
caught, its path arguments, fake root, result, effect) and `policy_deny` in
`policy.c` (what the platform-killed calls are told). It had not been audited;
this is the first pass, done by comparing it against the arm64 syscall set
([`syscalls-by-kernel.md`](syscalls-by-kernel.md)) and the device census
([`device-census.md`](device-census.md)), and confirming with small programs
under `dn-tiny`.

**How to tell "killed" from "allowed"** (used throughout): run the call under
`dn-tiny`; if its number appears in the tracer's `answered for the platform's
filter` line it was killed (`SIGSYS`, answered `ENOSYS` or by `policy_deny`); if
it is absent, the kernel ran it.

## PATH_ARG completeness

The riskiest dimension: a path-carrying call the rows miss sends a **guest** path
to the kernel. Every arm64 syscall that takes a path argument, and its status:

| syscall | handled? |
| --- | --- |
| openat, openat2, faccessat, faccessat2, fchmodat, fchmodat2, fchownat, newfstatat, statx, readlinkat, linkat, symlinkat, unlinkat, mkdirat, mknodat, renameat, renameat2, utimensat, chdir, [l]setxattr, [l]getxattr, [l]listxattr, [l]removexattr, inotify_add_watch | a row (maps the path) |
| execve | a row (the exec gate) |
| execveat (281) | a row (the exec gate; reads the dirfd, flags, and AT_EMPTY_PATH) |
| mount, umount2, swapon, swapoff, chroot, init_module, delete_module, finit_module, name_to_handle_at, open_by_handle_at, open_tree, move_mount, fsopen, fsconfig, fsmount, fspick, mount_setattr, acct, kexec_file_load, kexec_load | killed off the app's filter -> answered (`EPERM`/`ENOSYS`); no path reaches the kernel |
| pivot_root (41), quotactl (60) | **allowed, not caught**: the kernel runs them with the guest path |
| fanotify_mark (263) | killed -> `ENOSYS` (checked, not assumed) |
| AF_UNIX `bind`/`connect`/`sendto`/`sendmsg` paths | not caught (known, `TODO.md`, "The law") |
| setxattrat/getxattrat/listxattrat/removexattrat, statmount, listmount | newer than either device |

### `execveat` (281) — the one real gap, now fixed

It was allowed by Android and not caught, so the guest path reached the kernel: a
program calling `execveat(AT_FDCWD, "/usr/bin/true", ...)` got **`-1 ENOENT`**,
and `fexecve` skips the loader rewrite. Fixed with a row for 281 through the exec
gate, which now reads execveat's `dirfd` (x0), `pathname` (x1), `argv` (x2) and
`flags` (x4), resolves a relative name from the dirfd, and handles `AT_EMPTY_PATH`
by taking the path the fd names (so `fexecve` works). Measured: the path form and
`fexecve` both run; the Lua build is unchanged (1666 stops).

### The minor ones

- `pivot_root` (41) and `quotactl` (60): allowed, not caught. Measured, the
  kernel runs them with the guest path (`EPERM`, `ENOENT`). Both are privileged,
  so they fail anyway; the only effect is that a guest path reaches the kernel.
- `capget` (90) is not faked: a program that probes capabilities sees the real
  set (measured `eff=0`), not root's. To decide.

## Over-catch (rows that never run)

- **`chroot` (51)** is both a row (a path) and in the deny table (`EPERM`).
  Android kills it, so the SIGSYS path answers it and the row's path mapping is
  dead. The row can go.
- **`fchownat` (54)** is in `PATH_ARG` (`path_arg=1`) and in `FAKE`;
  `trace_enter` returns at the fake, so its `path_arg` is dead.
- `symlinkat` (36): `PATH_ARG(2,)` and `EFFECT(1)` — both used.

## FAKE

`trace_denied` **also** applies a row's fake (`if (rw && rw->fake != F_NONE)
fake_result(...)`), so a call Android *kills* still gets its fake. That matters
for the `set*id` family, which is killed: measured, `setuid(1000)` then `getuid()`
returns 1000 (`F_SETID`), and `setresuid(1,2,3)` reads back 1,2,3. So those rows
are live. The rest measured too: `getgroups` -> one group 0, `getresgid` ->
0,0,0, `fchown(0,0,0)` -> 0. Complete; `capget` (not faked) is the one gap.

**The audit is complete** (PATH_ARG, FAKE, RESULT, EFFECT, deny); the outcome
below is the summary.

## RESULT

`getcwd` (17), `readlinkat` (78), `newfstatat` (79), `statx` (291) rewrite their
result. These are the only path-shaped returns; `fstat` (80) is deliberately not
caught (its comment in `gen_rows.py`). Complete.

## EFFECT

`E_LINKS` on `unlinkat`/`symlinkat`/`linkat`/`renameat`/`renameat2`; `E_CWD` on
`chdir`/`fchdir`. The other calls that change the tree are refused (`mount`,
`umount2`, `chroot`). Complete.

## deny vs the census

`device-census.md`'s 32 real denials each have an answer: the `set*id` family and
`setgroups` -> succeed (or the fake above); `mount`, `umount2`, `chroot`,
`swapon`, `swapoff`, the clock and hostname setters, the module and `bpf` calls
-> `EPERM`; `accept` -> `accept4`, `faccessat2` -> `faccessat`, `fchmodat2` ->
`fchmodat`, `openat2` -> `openat`; the rest -> `ENOSYS`. Consistent.

## Which rows the shim can take in-process

The shim's fast path answers a call in the process (the same syscall, its path
resolved) when nothing else is needed; the site is found by a window scan and the
hook reads `x8` ([`stability.md`](stability.md)). Over the 52 rows:

- **Directly (25).** The read-only path calls (`openat` read, `faccessat`,
  `newfstatat`, `statx`, `readlinkat`, `getcwd`, `[l]getxattr`, `[l]listxattr`)
  and the path-only writes (`mkdirat`, `mknodat`, `fchmodat`, `utimensat`,
  `inotify_add_watch`, `statfs`, `truncate`, `set`/`lset`/`remove`/`lremove`
  xattr). Pure mapping: no state, no cache.
- **Tried and rejected: the link movers (4).** `unlinkat`, `symlinkat`,
  `renameat`, `renameat2` (and `linkat`, which needs the copy the tracer does) —
  moved to the shim with a bump of the shared link-cache generation. It **hurt**:
  the Lua build went from **1662 to 1708 stops**, because dropping the shared
  cache on every link move makes the tracer re-resolve paths elsewhere. Reverted.
  (The generation is cheaper to drop from one place than from two.)
- **The working directory (2).** `chdir`, `fchdir` change the cwd the tracer keeps
  per process; the shim would have to make that change visible. Not done.
- **Not this trick.** `execve`/`execveat` (the exec gate rewrites the call), the
  `set*id` family and `getres*`/`getgroups`/`fchown`/`fchownat` (a per-process id
  store), `openat2`/`faccessat2`/`fchmodat2` (Android kills them; the tracer
  substitutes), `chroot` (refused). `get*id` are already answered by the shim from
  the tracer's copy.

So the same trick could take **7 more** (the link and cwd calls) once the shim and
the tracer share cache invalidation; the rest need the gate or the id store.

## Outcome

- **One real bug found and fixed: `execveat`** (a row for 281 through the exec
gate; the path form and `fexecve` now run).
- Minor: `pivot_root`/`quotactl` (allowed, unmapped, privileged), `capget` (not
  faked).
- Cleanup: the dead `chroot` row and `fchownat` `path_arg`.
- Everything else checked and correct; the numbers are stable across kernels, so
  the rows are portable.
