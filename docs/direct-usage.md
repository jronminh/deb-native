# Direct syscall usage — investigation

Living doc. The *map* of the boundary is [`syscall-boundary.md`](syscall-boundary.md);
this is the working investigation into **what actually bypasses the libc shim
in the packages we support, and which mechanism should reach it**. Results land
here first, then get promoted into the boundary doc / `design.md`.

## The decision this doc is for

Which mechanism reaches the access libc interposition cannot:

1. extend the existing **`proot`** route (syscall-level, already wired for
   static in `native/dn-run.c`);
2. add a **`syscall()` interposer** to the shim (cheap; covers only the
   explicit-wrapper case);
3. build a **`ptrace` / `SECCOMP_RET_USER_NOTIF` tracer** (the endgame).

## Questions

- **Q1** — For the in-scope **programs** on `PATH` (not the libraries), what is
  the per-case breakdown? (The 258-package corpus is 415/668 libraries, so the
  current numbers over-weight the wrong population.)
- **Q2** — Does a `syscall()` interposer in the shim catch the binaries that
  *import* `syscall`, and at what cost? (It is a public symbol, so this should
  be possible without `ptrace`.)
- **Q3** — What is `proot`'s measured overhead on a hot path (a real Debian
  program from the prefix), before choosing option 1 over option 3?
- **Q4** — What per-binary attribute should drive routing? `PT_INTERP` alone is
  not enough: `abbtr` is a dynamic PIE that emits `svc #0` itself and is
  currently routed to the shim.

## Method / tools

- `scripts/scan-direct-syscalls.py DIR --verify --list` — ELF classification +
  candidate filter + `objdump` verification (a byte grep gives 111 false
  positives where disassembly gives 6).
- `tests/shim-libc/` — the on-device harness; extend it for the `syscall()`
  test (Q2).
- `native/dn-run.c` — the current `PT_INTERP` classifier.
- Corpus on the phone (from [`shim-coverage.md`](shim-coverage.md)):
  `~/debcorpus/{Packages.gz,sel.tsv,debs/,root/}`.

Reproduce (on `fe2`, one command at a time):

```
python3 ~/deb-native/scripts/scope-sample.py ~/debcorpus/Packages.gz > ~/debcorpus/sel.tsv
find ~/debcorpus/debs -name '*.deb' -exec dpkg-deb -x {} ~/debcorpus/root \;
python3 ~/deb-native/scripts/scan-direct-syscalls.py ~/debcorpus/root --verify --list
```

## Solutions

### Existing: `proot` (Termux's)

`native/dn-run.c` route #2 execs `proot -b <instdir>/<dir>:/<dir>` for
binaries with no `PT_INTERP`. It is a `ptrace` syscall interceptor: it traps
each syscall and rewrites the path arguments, so it is *syscall-level* and
therefore covers cases 2–5 (libc-internal / static bindings, inline `svc`,
explicit `syscall()`, and static executables) in one mechanism. That is why the
project reaches for it rather than building a tracer first.

Its costs and limits, all visible in the code or its model:

- **Tracing overhead** — `ptrace`, but filtered by `seccomp-bpf` to only the
  syscalls it translates (below), so it is not a stop on *every* syscall. Q3
  still measures the real cost.
- **`-b` needs the host path to exist** (`dn-run.c` binds only dirs that
  `stat()`); a missing guest path is an error, not an empty view.
- **External dependency** — Termux's `proot` binary, against the project's
  stated no-`proot` ideal; it is a pragmatic fallback, not the design.
- **A fixed syscall table** — checked against proot's source
  (`termux/proot` `src/syscall/enter.c` and `exit.c`). The path-handling set is
  broad and current: `open`/`openat`/**`openat2`**,
  `stat`/`stat64`/`newfstatat`/`fstatat64`/**`statx`**, `access`/**`faccessat2`**,
  `creat`, `readlink`/`at`, `chdir`/`fchdir`/`getcwd`, `mkdir`/`at`,
  `mknod`/`at`, `unlink`/`at`, `rmdir`, `rename`/`at`/`at2`, `link`/`at`,
  `symlink`/`at`, `chmod`/`fchmodat`, `chown`/`lchown`/`fchownat`,
  `truncate`/`64`, `utime`/`utimes`/`utimensat`/`futimesat`,
  `statfs`/`64`, the xattr family, `inotify_add_watch`, and the socket calls
  `bind`/`connect`/`sendto`/`sendmsg`/`recvfrom`/`recvmsg`. It runs `ptrace`
  accelerated by a **`seccomp-bpf` filter** so only the syscalls it cares about
  are trapped (with a `PROOT_NO_SECCOMP` fallback), which is why its overhead
  is not the naive per-syscall figure.
- **What it does *not* translate** — the **`io_uring`** syscalls are in neither
  table, so an `IORING_OP_OPENAT` (path in a shared ring, not a syscall
  argument) is not rewritten; also the new mount API
  (`fsopen`/`open_tree`/`move_mount`/`mount_setattr`), `open_by_handle_at`
  (only `name_to_handle_at` is present) and `fanotify_mark`. For this project
  the mount API and fanotify are out of scope; **`io_uring` is the one to
  verify, and Android's own seccomp may already block it for apps** — which
  would make it moot.

### Direct-usage: interposer + tracer

Two layers, cheapest first:

1. **`syscall()` interposer in the shim** (case 3). `syscall()` is a public
   symbol, so the shim can add one function that reads the syscall number and
   rewrites the path argument for the open/stat family — no `ptrace`, no
   `proot`. It covers only callers that *import* `syscall` (Q2); inline `svc`
   is invisible to it.
2. **A purpose-built tracer** (`SECCOMP_RET_USER_NOTIF` or `ptrace`) for cases
   2/4/5. It is the endgame: it can replace `proot` (same coverage, in-process,
   no external binary) and, unlike a syscall-argument rewriter, is where an
   `io_uring` answer would have to live.

Coverage, by case:

| case | shim | `syscall()` interposer | `proot` | own tracer |
|---|---|---|---|---|
| 1 libc path call | **yes** | — | yes | yes |
| 2 libc-internal / static binding | no | no | **yes** | **yes** |
| 3 explicit `syscall()` | no | **yes** | yes | yes |
| 4 inline `svc #0` | no | no | **yes** | **yes** |
| 5 static executable | no | no | **yes** | **yes** |

So "existing" and "direct-usage" are not rivals: `proot` already *is* a
direct-usage solution. The decision is whether to keep leaning on it
(option 1), add the cheap shim wrapper (option 2), or build the in-process
tracer that makes the no-`proot` ideal true (option 3).

## Experiment log

| date | experiment | result | conclusion |
|---|---|---|---|
| 2026-09-26 | full corpus scan (258 pkgs → 668 ELFs) | 7 `ET_EXEC`, 246 PIE, 415 `.so`; 6 emit `svc`; 12 import `syscall` | counts small; the structural problem is routing (`abbtr`) |
| — | Q1: in-scope programs only | *pending* | — |
| — | Q2: `syscall()` interposer | *pending* | — |
| — | Q3: `proot` overhead | *pending* | — |
| 2026-09-26 | Q4b: proot syscall coverage (source check) | `openat2`/`statx`/`faccessat2`/xattr/sockets handled; `io_uring` absent | `io_uring` is proot's blind spot; verify Android's seccomp blocks it |

## Working notes

- The boundary doc's cases: (1) libc path call → shim; (2) libc-internal /
  statically-bound (NSS) → unreachable; (3) explicit `syscall()` → shim *could*;
  (4) inline `svc #0` → tracer; (5) fully static exe → `proot`/tracer.
- `libc.so.6` exists in the glibc bundle, so "static libc" in the sense of
  *statically-bound symbols inside a dynamic libc* (case 2) is the Termux-relevant
  one; a fully static executable (case 5) is what `proot` already covers.
- Binaries found so far: `busybox`, `bash-static`, `abbtr` (PIE + 38 `svc`),
  Go `arduino-builder`/`balloon`/`c2go`.

## Decision

*Pending.* Working lean: **option 1** (extend the `proot` route) is the
cheapest and reuses the tool already trusted; **option 3** is the endgame once
option 1's overhead is known. Option 2 is a cheap probe worth doing for Q2.

## Next step

- [ ] Q1: re-run the scan restricted to the executables a package puts on
      `PATH` (`usr/bin`, `usr/sbin`, `usr/games`), and map each to its package,
      so the breakdown is per supported program, not per ELF in the tree.
