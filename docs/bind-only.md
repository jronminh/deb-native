# Bind-only fork-lite — what is safe to remove

Audit of `tracer/` (fork-lite, the reduced arm64-only PRoot) to turn it into a
**bind-only** path tracer suited to this project's scope. Findings only; no
code changed. Method: read the actual tree, cite `file:line`. See
[`direct-usage.md`](direct-usage.md) for the fork-lite plan and
[`syscall-boundary.md`](syscall-boundary.md) for why syscall-level rewriting is
needed at all.

## What "bind-only" means

Today `translate_path` (`path/path.c:318`) canonicalizes every path:
`path/canon.c:199` walks component by component, `lstat()`s each
(`canon.c:159`), resolves symlinks, and re-applies bindings. That is the hot
cost.

Bind-only replaces that with a **prefix rewrite**: match the leading component
against the flat bind table (`/usr /etc /var /opt /bin /sbin` →
`$INSTDIR/<dir>`), prepend the host prefix, and let the kernel resolve the
rest. rootfs is `/` (no chroot), bindings are top-level dirs, no nesting or
asymmetric binds. The base resolution (dir_fd/cwd), `join_paths`, and
`substitute_binding` stay.

## Call graph of the removal

`canonicalize` callers (excluding its own recursion at `canon.c:366`):

| caller | purpose | bind-only |
|---|---|---|
| `path/path.c:373` | translate_path | **REPLACE** with prefix rewrite |
| `path/binding.c:596` | canonicalize a binding's guest side at init | **REPLACE** with plain normalize |
| `syscall/enter.c:236` (`guest_canonicalize`) | mount/pivot_root (`:287`, `:321`, `:448`) | **DELETE** with mount handling |
| `cli/cli.c:246` | initial cwd | new binder |

## Green — safe to delete

- **`path/f2fs-bug.c` / `.h`** — one call site, `canon.c:156`, inside
  `substitute_binding_stat`. Dead once canonicalize goes.
- **`path/glue.c` / `.h`** — `build_glue` is called only from `canon.c:174`;
  the `glue_type` field (`binding.c:579,610`) is consumed only by
  glue/canon (`canon.c:148,173,348`). All dead.
- **`readlink_proc()` (`path/proc.c:43`)** — called only from `canon.c:313`.
  `proc.c` has no shared statics; delete the function, **keep**
  `readlink_proc2` (`proc.c:190`, used by `detranslate_path` at `path.c:434`).
- **mount/umount/pivot_root/unshare/setns** enter+exit handling and the
  `guest_canonicalize` helper (`enter.c:223`; cases `enter.c:2410-2499`,
  `exit.c:244-249`). Out of scope (`TODO.md`, "Blocked").
- **`translate_path2_parent` (`enter.c:150`)** — exists only because
  canonicalize probes the final component, which must not exist for
  `mkdir`/`rename` targets. Prefix-rewrite never probes, so all call sites can
  use `translate_path2`.

## Yellow — replace, do not just delete

- **`path/canon.c`** — write `translate_path_inner()`: strip `//`/`.`,
  fall back for `..`, match the leading bound dir, prepend `$INSTDIR`,
  otherwise pass through. Keep `canonicalize` as the **fallback** for paths
  with `..` or an undecidable symlink component.
- **`path/binding.c:initialize_binding` (`:547`)** — replace the
  `lstat`/`canonicalize` block (`:569-610`) with a simple absolute-normalize.
  Keep `get_binding`/`substitute_binding` (`binding.c:120,239`) verbatim.
- **getcwd (`exit.c:95`)** — enter voids the real syscall (`enter.c:1905-1909`)
  and exit rebuilds from `tracee->fs->cwd` (`exit.c:112`), calling
  `translate_path(".",false)` on **every** getcwd (`exit.c:108`) just to test
  existence. Under bind-only, let the kernel's getcwd run and
  `detranslate_path` the result.

## Red — keep

- **`ptrace/`, `tracee/`, `loader/`, `execve/`** — independent of
  canonicalization. exec/loader injection is why `brk` tracking exists
  (`syscall/heap.c:54,150`; `execve/enter.c:672`, `execve/exit.c:497`). Do not
  drop `brk` without replacing it with a lazy `/proc/<pid>/maps` read.
- **`syscall/heap.c`, `chain.c`, `rlimit.c`** — brk/heap, injected-syscall
  chaining, the stack-limit kernel-bug workaround (`rlimit.c`).
- **`path/temp.c`** — used by execve/socket/enter, not canon-only.
- **`extension/extension.c` + `notify_extensions`** — removing the call sites
  is intrusive; with zero extensions they are empty-list no-ops. Leave them.
- **`path/proc.c:readlink_proc2`**, **`readlink_proc_pid_fd`** (`path.c:287`;
  used at `execve/enter.c:521`, `exit.c:397`, `tracee/statx.c:31`,
  `path.c:332`).

## Syscall whitelist (`syscall/seccomp.c:331`)

Orthogonal to bind-only: dropping canonicalize does not shrink the traced set.
Perf-relevant entries:

| entry | verdict | note |
|---|---|---|
| `close` (`:345`) | MAYBE/DROP | handler `enter.c:2875` only untracks netlink + `shadow_pipe_read_end` (`enter.c:2892`). Dropping also drops `syscall/pipe_shadow.c` (`enter.c:2892`, `tracee/event.c:398`). Test `bash` process substitution first. |
| `brk` (`:337`, FILTER_SYSEXIT) | KEEP | needed for exec injection; 2 stops/call is the biggest remaining cost — optimize via `/proc/maps`, do not delete. |
| `ioctl` (`:368`) | KEEP | Termux termios/`SIOCGIFINDEX` (`enter.c:2811`). |
| `prctl` (`:397`) | KEEP | `no_new_privs`/dumpable/seccomp-block (`enter.c:2755`). |
| socket family (`:332-352, :409`) | MAYBE | AF_UNIX path translation only; `/run` unbound here. Low frequency. |
| xattr family (`:365-378, :402`) | KEEP | path-taking; `cp -a`/`tar`. |
| `statfs`/`statfs64` (`:412`) | KEEP | path arg + exit translation (`exit.c:684`). |
| `wait4`/`waitpid` (`:432`) | MAYBE | `translate_wait_enter` (`enter.c:1895`); check if `-k`-only. |
| mount/umount/pivot_root/unshare/setns, swapoff/swapon, acct, chroot, uselib, name_to_handle_at | DROP | out of scope; removes handlers, negligible perf. |

The actual bind-only surface — **KEEP**: `open/openat/openat2`,
`stat/newfstatat/lstat`, `access/faccessat2`, `readlink/readlinkat`,
`chdir/fchdir/getcwd`, `mkdir/mknod`, `unlink/rmdir`, `rename`, `link`,
`symlink`, `chmod/chown`, `truncate`, the `utime` family.

## Risks — what canonicalize was silently handling

- **Absolute symlinks** — `canon.c:340` readlinks, `:357` detranslates,
  `:366` re-canonicalizes, then re-applies bindings. Prefix-rewrite lets the
  kernel resolve an absolute target against the real host `/`. Mitigate by
  normalizing the prefix's absolute symlinks to relative at install time and
  falling back to canonicalize when a component is a symlink.
- **`..` across a bind** — `pop_component` (`canon.c:44,248`) clamps in guest
  space; prefix-rewrite maps `/usr/..` to host `$INSTDIR`. Detect `..` and
  canonicalize.
- **Output detranslation** — `detranslate_path` (`path.c:402`) +
  `readlink_proc2` (`proc.c:190`) stay; must strip `$INSTDIR` from
  getcwd/readlink/`/proc/self/cwd` results.
- **`/proc` magic links** — `readlink_proc` (canonicalize-only) goes;
  `readlink_proc2` covers the output side. Re-test `/proc/self/cwd`,
  `/proc/self/root`.
- **dir_fd-relative paths** — the base branch (`path.c:330`) already yields a
  host dir, so the relative part needs **no** rewrite. A fast win, not a risk.

## Status (2026-09-26, implemented)

The core bind-only path plus the safe mechanics for the three traps landed:

- **Fast path** — `translate_path` (`path/path.c`) normalizes the guest path
  (collapse `.`/`//`, keep one trailing `/`) and prefix-substitutes, skipping
  per-component `lstat`.  `normalize_guest_path()` returns -1 on `..`, which
  falls back to `canonicalize()` (trap 2).  Disable with `PROOT_NO_BIND_ONLY=1`
  to force the old path (A/B).
- **Trap 1 (absolute symlinks)** — `scripts/normalize-symlinks.sh` rewrites
  absolute targets under bound dirs to relative, idempotently (5-pass cap).
  Wired into `install.sh` after install.  Demonstrated: an absolute `/etc/x`
  symlink fails under bind-only, passes after normalizing.
- **Trap 3 (detranslation)** — unchanged: `detranslate_path` still strips the
  host prefix for getcwd/readlink/`/proc/self/cwd`.  Verified.
- **Verified on `fe2`**: bind read; `..` clamp (`/etc/../../etc/x`) matches
  canonicalize; absolute-symlink before/after; `/proc/self/cwd` -> `/etc`.
- **Benchmark** — `scripts/bench-tracer.sh`, medians on `fe2`, binds
  `$PREFIX:/usr`:

  | workload | og (stock proot) | fork-lite canonicalize | fork-lite bind-only |
  |---|---|---|---|
  | 20 000 path lookups, one process | 2.26s | 2.26s | **1.40s (~1.6x)** |
  | `find -type f` over 67k files | 2.66s | 2.80s | 2.45s (~8%) |

  og ≈ fork-lite-canonicalize, so the gain is the fast path, not the extension
  prune; the walk is I/O-bound.
- **Not done**: getcwd kernel-passthrough, whitelist pruning, and deleting
  `glue.*`/`f2fs-bug.*`/`readlink_proc` — kept because canonicalize is still
  the fallback.

## Scope — our proot vs general proot

The fast path is a **deb-native optimization, not an upstream proot
improvement**. It is safe only under assumptions stock proot does not make:

| our fork-lite assumes | general proot must support |
|---|---|
| rootfs `/` (no `-r` chroot) | chroot rootfs; `..`/symlinks clamp to it |
| flat top-level binds | nested/asymmetric binds, bind-into-bind |
| guest tree symlink-normalized by `install.sh` | resolves absolute symlinks itself |
| no path-manipulating extensions | `hidden_files`, `link2symlink`, `fake_id0`, `kompat` hook every path |

Break any of these and prefix-rewrite answers wrongly (the symlink/`..` traps).
The escape hatches keep it *safe*, not *optimal*: `..` auto-falls back to
`canonicalize()`, and `PROOT_NO_BIND_ONLY=1` restores full behavior.

What stays general: the `ptrace` core, syscall enter/exit tables, exec/loader
injection, detranslation. What is ours: the fast path plus the symlink
normalizer (a property of our prefix, not a proot feature). Upstreaming it
would mean an opt-in fast translate with a caller-maintained symlink invariant
and a canonicalize fallback — a larger design change.

## Suggested order

1. Write `translate_path_inner` (prefix rewrite + `..`/symlink fallback)
   behind a flag so it can be A/B'd against canonicalize.
2. Fix binding init; delete `glue.*`, `f2fs-bug.*`, `readlink_proc`,
   `canon.c`-only helpers.
3. Replace `cli/` with `dn-trace` (register binds, set `DN_INSTDIR`/`PATH`,
   `chdir`).
4. On `fe2`: absolute-symlink + `..` test matrices, then benchmark against
   stock `proot`.
5. Only then touch the whitelist — start with `close`/`pipe_shadow`, measure.
