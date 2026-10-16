# Policy audit

The policy is two places: `src/dn-tiny/gen_rows.py` (the rows: which syscall is
caught, its path arguments, fake root, result, effect) and `policy_deny` in
`policy.c` (what the platform-killed calls are told). It had not been audited;
this is the first pass. Method: compare it against the arm64 syscall set
([`syscalls-by-kernel.md`](syscalls-by-kernel.md)) and the device census
([`device-census.md`](device-census.md)), and confirm with small programs under
`dn-tiny`.

## PATH_ARG completeness

The riskiest dimension: a path-carrying call the rows miss sends a **guest**
path to the kernel.

- **`execveat` (281): not caught, and it is broken.** The exec gate handles
  `execve` (221) only. Measured: a program calling
  `execveat(AT_FDCWD, "/usr/bin/true", ...)` under `dn-tiny` gets **`-1 ENOENT`**
  (the guest path reaches the kernel). `fexecve` also uses `execveat` (with
  `AT_EMPTY_PATH`, so no path, but it is also not gated, so the loader rewrite is
  skipped). **Fix: a row for 281, through the exec gate.**
- **`fanotify_mark` (263): not caught** (it takes a path and a dirfd). The census
  does not kill it, so a guest path reaches the kernel. To confirm.
- **`pivot_root` (41)** (two paths) and **`quotactl` (60)** (a path): not caught;
  the census does not kill them. Both are privileged, so they fail anyway; minor.
- **AF_UNIX socket paths** (`bind`, `connect`, `sendto`, `sendmsg` with a
  `sockaddr_un`): not caught — a known gap (`TODO.md`, "The law").
- `setxattrat`/`getxattrat`/`listxattrat`/`removexattrat` and
  `statmount`/`listmount`: newer than either device (see the kernel table); no
  row yet.

Answered through `policy_deny` already (so no path reaches the kernel): the new
mount API (`open_tree`/`move_mount`/`fsopen`/`fsconfig`/`fsmount`/`fspick`/
`mount_setattr`), `name_to_handle_at`, `open_by_handle_at`, `mount`/`umount2`/
`swapon`, `finit_module`.

## Over-catch (rows that never run)

- **`chroot` (51)** is both a row (a path) and in the deny table (`EPERM`).
  Android kills it (`SECCOMP_RET_TRAP` outranks `TRACE`), so the SIGSYS path
  answers it and the row is dead. The row can go.
- **`fchownat` (54)** is in `PATH_ARG` (`path_arg=1`) and in `FAKE`;
  `trace_enter` returns at the fake, so its `path_arg` is dead.
- `symlinkat` (36): `PATH_ARG(2,)` and `EFFECT(1)` — both used.

## FAKE

Asked of the man pages (next): the `F_*` semantics (`F_GETID`=5, `F_SETID`=6,
`F_RESIDS`=2, `F_GROUPS`=3, `F_ZERO`=1), and any fake-root call the rows miss
(`capget`, `getgroups` variants, `getresuid` set).

## RESULT

`getcwd` (17), `readlinkat` (78), `newfstatat` (79), `statx` (291) rewrite their
result at exit. To check: any other call that returns a guest-path-shaped buffer
(`fstat` (80) is deliberately not caught, per the comment in `gen_rows.py`).

## EFFECT

`E_LINKS` on `unlinkat`s and the link calls; `E_CWD` on `chdir`/`fchdir`. To
check: any other call that changes a link or the working directory without
dropping the cache (`pivot_root`, a `mount` if it were allowed).

## deny vs the census

`device-census.md` lists the 32 calls the app's filter really kills on the S10e.
Each should be answered by a row or by `policy_deny` (the defaults return
`ENOSYS`, which a program that probes for a newer call expects). To do: go
through the 32 and mark each.

## What is already right

- The syscall **numbers are stable** across kernels
  ([`syscalls-by-kernel.md`](syscalls-by-kernel.md)), so the rows are portable.
- The deny table covers the privileged calls (`mount`, the module and hostname
  setters, ...) and substitutes `accept` -> `accept4`, `faccessat2` ->
  `faccessat`, `fchmodat2` -> `fchmodat`, `openat2` -> `openat`.
