# Runtime revision

Status: **proposal, for discussion** (2026-10-19). Not current truth. This
records a working direction: rebuild the runtime on a two-part foundation --
`dn-init` (the catcher, real init, and the prefix's kernel) and `dn-exec`
(userland exec) -- with the shim reduced to a context-fed fast-path client and
the law owned once. The evidence and the open questions are below.

## Why

The runtime today applies one law through three self-booting engines, each of
which re-derives the context it needs:

- `dn-tiny` -- the tracer/catcher (ptrace + seccomp), the exec gate, identity,
  and `/proc` synthesis.
- `dn-lane.so` -- the shim, a glibc DSO preloaded into every program; it
  interposes symbols and patches libc's `svc` sites so read-only calls cost no
  stop.
- `dn-ldhook.so` -- a loader hook (`LD_AUDIT`) that patches the loader's own
  `openat`/`faccessat` sites before the shim is loaded.

The law (`policy.c`) is compiled three ways (`-DDN_LIBC`, `-DDN_FREE`, and as the
tracer); root is discovered three ways (`-t ROOT`, `dladdr`, `/proc/self/maps`);
`boot/binds` and the link generation are parsed in three places; the artifact
paths are hard-coded in five; the lane cookie in three. The feature inventory is
in `docs/reference/feature-inventory.md`; the duplication list is section
[Duplication](#duplication) below.

Two experiments establish what is load-bearing and what is speed:

- The shim stopped working under `dn-exec` because it derived root from its own
  path and `dn-exec` hands the loader a guest path. A root-from-maps fallback
  fixed it (`lane.c`); after the fix `dn-exec` matched kernel-exec: `find /usr
  -type f` stops 5758 vs 5752, wall equal, output byte-identical.
- Removing `dn-ldhook.so` changed nothing about correctness: `find /usr -type f`
  produced identical output with the loader's opens going through the law/tracer;
  ldhook only saved ~6 stops (5752 -> 5758 entry). It is speed, not correctness.

So the shim and the loader hook are optional fast paths; the only load-bearing
pieces are the tracer (the law), the loader, and libc. That is the basis for the
revision.

## The four foundation ideas

| idea | embodiment |
| --- | --- |
| one catcher that supervises every process and answers its syscalls | `dn-tiny` today; `dn-init` in the revision |
| a real init as the base for process management | new: replaces `init.sh`; folded into the catcher |
| the illusion that the Android kernel lives inside the prefix root | `mapped-kernel.md`: the law + `/proc` synthesis + the device table |
| disguise the Android kernel as a stock arm64 kernel | `phase2.md` + `contract.md` |

The catcher and the init are one process: it is PID 1 of the prefix, the ptrace
parent of everything, the seccomp supervisor, and the kernel of the prefix. Its
`waitpid`/ptrace loop is already the init loop -- PID 1's job (reap and supervise
the process tree) is what the loop does -- so no second loop is needed; only a
small state machine at the edges (what to spawn first, whether to respawn on
session end, signal handling).

## Target structure

```
src/
  dn-init/          # catcher + real init: PID 1, ptrace+seccomp, the law, /proc
    catch.c         #   the wait/ptrace loop, the filter, the lane
    law.c           #   the path table + walk (single owner; from policy.c)
    gate.c          #   the exec gate = the virtual execve
    apply.c         #   syscall answers, identity, /proc synthesis
    init.c          #   init: session, signals, reap, respawn (from init.sh)
    rows.inc        #   the generated syscall catalog
  dn-exec/          # birth: map the loader (or a static program), build
    dn-exec.c       #   stack/auxv, jump; a dumb executor, no policy
  dn-lane/          # the shim: a thin, context-fed fast-path client
    lane.c
  boot/
    boot.sh         # find root, clean env, exec dn-init
  spec/             # the contract: promised / best-effort / refused
```

`dn-ldhook` disappears. If the loader's startup stops still matter, that patching
belongs in `dn-exec` at map time, not in a self-booting audit module.

## The law becomes data

Today `policy.c` is compiled three times. In the revision the law is a **table
owned by `dn-init`**, mmap'd read-only into every child; the shim is a generic
reader of that table, not a second copy of the law. This removes the "two copies
disagree" risk and the three-way `#ifdef` build, and it is how the codebase gets
"into the right place": correctness lives in `dn-init`, speed in `dn-lane`, birth
in `dn-exec`.

## The fast path

"Fast path" is not the shim: it is the property that a syscall is answered
faster than the catcher's ptrace round-trip. There are exactly three places a
syscall is answered:

| level | where | cost | reach |
| --- | --- | --- | --- |
| 1. filter | seccomp returns ALLOW / ERRNO | 0 stop | every program, static included, but fixed results only (no path mapping) |
| 2. in-process | code in the process maps the path and re-issues the call | 0 stop (via the lane cookie) | dynamic programs only (needs a linker to inject code) |
| 3. catcher | ptrace wakes, maps, resumes | 1 stop | everything (the net) |

The shim is level 2 delivered by LD_PRELOAD, so it reaches only dynamic glibc
programs and only the symbols/sites it interposes or patches. Static programs,
foreign libcs, the loader, and direct syscalls fall to level 3. The shim is one
narrow fast-path mechanism, not the fast path.

The division of labour between shim and tracer is not declared anywhere: it is
implicit in four lists that must agree -- `gen_rows.py` (what the tracer
catches), `lane.h` `DN_LANE_NRS` (what the shim answers and what the cookie
admits), `policy.c`'s deny table (what a denied call is told), and `lane.c`'s
interposed symbols and patched sites. History shows the cost of that implicit
overlap: the cookie leak (`0321cb1`) and the two-laws divergence the 0.7 runtime
died of (`a68567a`).

In the revision the fast path is a first-class, three-level notion installed at
birth by `dn-exec`, uniformly for every program type: level 1 for fixed answers
(the platform-denied syscalls belong here as `RET_ERRNO`, not as SIGSYS stops),
level 2 where a linker exists, level 3 as the net. The lane cookie then stops
being a shared secret that bypasses the law and becomes a scoped contract
between the foundation and the filter.

Level 2 needs a dynamic linker to inject code, so a static program has no
in-process fast path: only levels 1 and 3 apply. The fast path cannot replace
the catcher for everything; it is three clear levels, and each program falls to
the level it belongs to.

### Fast-path mechanisms (survey)

Known mechanisms per level, from `docs/reference/kernel-mechanisms.md`,
`prior-art.md`, `syscall-classes.md`, `inline-hooking.md`, and the E-series
experiments in the history:

| mechanism | level | reach | kernel | status |
| --- | --- | --- | --- | --- |
| vDSO (`clock_gettime`, `getcpu`) | 0 (no trap) | every program, already glibc | old | already used |
| seccomp `RET_ERRNO` for denials | 1 | every program | 3.5 | not done: denials are answered at `SIGSYS` today |
| constant-action bitmap (nr-first ordering) | 1 | every program | 5.11+ | blocked by the cookie reading `args[5]` |
| LD_PRELOAD shim (today) | 2 | dynamic glibc | - | narrow |
| patch every `svc` (svc-hook) | 2 | dynamic + loader + generic `syscall()` | - | wider than the `mov x8; svc` pair; could replace ldhook |
| `dn-exec` patch-at-birth | 2 | dynamic **and static** (the image it maps) | - | the generalization; static has no fast path today |
| gate page + instruction-pointer marker (E13) | 2 | dynamic | - | same stops as the cookie, smaller filter, reads IP each call; no gain |
| in-process SIGSYS handler (E12, systrap) | 2 | dynamic | 3.5 | failed: the handler does not survive exec (the loader's window) |
| `SECCOMP_RET_USER_NOTIF` | 3 | every program | 5.0 / `ADDFD` 5.9 | the kernel's supervisor; only the Android 16 phone |
| Syscall User Dispatch | 3 | every program | 5.11+ | neither phone |
| ptrace (today) | 3 | every program | 3.5 | the net |
| `process_vm_readv`/`writev` | catcher cost | every program | 3.2 | cheaper than PEEK/POKE |
| inotify | cache cost | - | 2.6.13 | a precise link-cache drop |

Conclusions:

- The one clear, cheap win is level 1: answer the platform-denied syscalls with
  `RET_ERRNO` in the filter instead of a `SIGSYS` stop (the 263 stops above).
- Level 2 widens two ways, both "patch the `svc` sites of the image `dn-exec`
  maps": patch every `svc` (replaces ldhook, reaches the loader and generic
  `syscall()`), and patch a static program at birth (a fast path static programs
  do not have today).
- The cookie is one of two markers for "already resolved"; the gate-page/IP
  marker (E13) is the alternative with no gain and a filter-size/cache trade-off.
  The deeper fix is not the marker but the ownership: `dn-exec` installs the fast
  path and the filter is generated to know it.
- Level 3 has `USER_NOTIF` as a ptrace replacement on the 5.10 phone, worth
  measuring; SUD is out of reach on both phones.

### Static binaries (survey (b))

`dn-exec` already maps a static program (L0), and it maps it by `pread` into
**anonymous** `MAP_ANONYMOUS` memory, then `mprotect`s it RX. So patching a
static binary at birth is a write into memory `dn-exec` already owns, **before**
the RX `mprotect` -- no `execmod`/`execmem` needed, unlike the shim, which
patches file-backed libc text and needs `execmod`.

- glibc-static and musl-static: `mov x8, #nr; svc #0` -- the same window-scan
  `lane.c`/`ldhook.c` already use.
- Go: `SVC #0` with the number in `x8` (`MOVD $nr, R8`), but (a) the sites sit in
  Go's own runtime and the pattern needs testing, (b) Go does not set `TPIDR_EL0`
  (gVisor #13362), which matters only for a marker scheme, not for the cookie,
  and (c) the text is large, so one stub page is not within `B` reach of every
  site -- it needs the veneer allocator `inline-hooking.md` already names.
- The static hook is freestanding (like `ldhook`, `-DDN_FREE`): the law + lane +
  root handed by `dn-exec`; it reads `x8` at call time, maps the path through the
  one law, sets the cookie, and re-issues the call. Fail-safe: a wrong site costs
  a plain `svc`, which the tracer answers.

Conclusion: the static fast path is feasible and cleaner than the shim (no
`execmod`); its cost is a stub/veneer allocator for large text and a separate Go
pattern check (Go deferred for now). This makes "patch the `svc` sites of the
image `dn-exec` maps" the one mechanism for the loader and for static programs.

### Next: POC (loader fast path, re-homed into dn-exec)

The first POC for this direction: move the loader-patching that `ldhook.c` does
today (an `LD_AUDIT` module that patches the loader's `openat`/`faccessat` sites
at run time) into `dn-exec`, so the loader is patched at birth, before the jump.
`dn-exec` already maps the loader into anonymous memory, so the patch is a write
into memory it owns -- no `LD_AUDIT`, no self-discovery, no `execmod`. This
absorbs the 1082 loader-bootstrap stops without a separate module.

- `dn-exec.c`: after `map_image` of the loader, patch its `openat`/`faccessat`
  sites to a freestanding stub (the law `-DDN_FREE` + the lane cookie + the root
  handed down), and stop setting `LD_AUDIT`.
- `build-dn-exec.sh`: build `policy.c` (`-DDN_FREE`) and the patch emitter into
  `dn-exec`.
- shim: keep `lane.c`'s root-from-maps fix (the loader's open of the shim then
  goes through the tracer, which is already proven correct).
- Verify: A/B `dn-exec` with `LD_AUDIT` vs `dn-exec` patched-at-birth -- same
  stops, `find` output byte-identical, no loader module.

Result (2026-10-20, p-dev): done. `dn-exec` now patches the loader at birth and
stops setting `LD_AUDIT`. `find /usr -type f` output is byte-identical (21967
lines); stops 5751-5759 entry vs 5752 for kernel-exec (parity). 100 x `true`
spawn: 306 entry vs 406 kernel-exec and 706 for the old `LD_AUDIT` path -- the
birth-time patch is not just parity, it is cheaper than the loader hook (no audit
module to load, no self-discovery). The patch is one reusable `patch_image(lo, hi)`,
so it extends to a static binary by the same call.

## Boot flow

1. `boot.sh` cleans the environment, finds the root, and `exec dn-init` once
   (root + session parameters).
2. `dn-init` initializes: the law table, binds, generation, the seccomp filter,
   `/proc` synthesis, the device table, the contract.
3. `dn-init` acts as init: it spawns the session (login shell or the user's
   command) through `dn-exec`.
4. It then stays in its one loop -- reap children, answer syscalls, manage
   signals and sessions.

## Run-a-program flow

1. The program calls `execve`/`execveat`; seccomp stops in the gate (in `dn-init`).
2. The gate resolves the guest path (shebang, symlinks), classifies static vs
   dynamic, checks permissions and errno, computes identity, and packages the
   context (root, ids, artifact fds, the law table).
3. The gate rewrites the syscall to `execve(dn-exec fd)` -- the kernel execs
   exactly one runtime artifact.
4. `dn-exec` reads the context, maps the tree's `ld.so` from an fd (or maps a
   static program), builds the loader's stack/auxv as the kernel would, and
   jumps. It makes no policy decision and discovers nothing.
5. `ld.so` runs unpatched in the guest frame: it opens the program, libc and the
   libraries by their baked-in FHS paths (`/etc/ld.so.cache`, `/usr/lib/...`),
   which the law maps. No glibc patch, no ldhook (history: the retired
   `dn-glibc-android.patch` proved stock libc6 runs once paths are mapped).
6. The shim, mapped by the foundation and fed the table + context, interposes
   symbols and issues lane calls. It no longer self-discovers root/binds/gen.
7. `dn-init` keeps supervising: syscalls, `/proc`, identity, reaping.

## Component changes

| component | today | revision |
| --- | --- | --- |
| gate (in dn-init) | rewrite execve to the loader | the virtual execve: resolve, permission, errno, shebang, fd, identity, context |
| `dn-exec` | mini-kernel: maps program+loader, `--root`, sets `LD_*` | dumb executor: maps loader (dynamic) or program (static), no policy, no discovery |
| shim | self-boots: root, binds, gen; interposes; patches libc | thin client: context-fed; interpose + lane only |
| `dn-ldhook` | self-boots, patches the loader | removed, or folded into `dn-exec` map-time |
| tracer state | parses `dn-exec` argv, synthesizes `/proc` | exec boundary = the kernel exec event of `dn-exec`; identity from the gate |
| law | compiled three ways | one table, owned by `dn-init`, mmap'd shared |
| artifacts | shim built inside the tree; `dn-exec` copied into `usr/local/bin` | runtime artifacts built and shipped with `dn-init`/`dn-exec` |

## Duplication

Grouped from the feature inventory; each collapses to one owner in the revision.

- Root/prefix discovery: tracer (`-t`), `lane.c` (dladdr + maps), `ldhook.c`
  (maps), `dn-exec.c` (`--root`), and the retired `__dn_prefix_init` -- five
  copies of one idiom.
- Artifact paths: `policy.c`, `lane.c`, `ldhook.c`, `dn-exec.c`, `exec.c` -- five.
- The law: compiled three times (`-DDN_LIBC`, `-DDN_FREE`, tracer).
- `boot/binds` parsing: tracer, `lane.c`, `ldhook.c` -- three.
- `dn-tiny.gen` mapping: tracer, `lane.c` -- two.
- Identity law (setid/getid): `trace.c`, `lane.c` -- two.
- `access()` world view: `trace.c`, `lane.c` -- two.
- stat owner + symlink-size fix: `trace.c`, `lane.c` -- two.
- hard-link copy + `st_nlink`: `trace.c` (session table), `lane.c` (per-process) -- two.
- `/proc/self/maps` self-locate: `ldhook.c`, `lane.c` -- two.
- raw-syscall/lane wrappers: `lane.c`, `ldhook.c`, `dn-exec.c` -- three.
- lane cookie constant: `lane.h`, `ldhook.c`, `dn-exec.c` -- three.
- loader/`dn-exec` token recognition: `policy.c`, `trace.c`, `exec.c`+`trace.c`.
- delivering shim/ldhook to the loader: `exec.c` (argv) vs `dn-exec.c` (env).
- the `DN_*` env contract: `boot.sh`, `exec.c`, `lane.c`, `init.sh`.

## Second syscall to emulate

`execve` is the first syscall the runtime takes over from the kernel. The
candidate for the second comes from `dn-tiny`'s own profile, not from glibc:

Remaining stops in the Lua build (`docs/performance.md`): `ld.so`'s opens 1082,
libc's cancellable `openat` 324, SIGSYS 263, fork/exec events 258, `execve` 195,
`unlinkat` 79, shim ids 20. The second bench: every spawned process costs ~5
stops (300 x `true` = 1548).

The next target is therefore **process creation (`clone`/`fork`/`vfork`)** -- the
other half of spawn, which `dn-exec` does not cover. Two levels:

- Cheap: answer the platform-denied syscalls (`set_robust_list`, `clone3`,
  `rseq`, `faccessat2`) in dn-init's seccomp filter with `SECCOMP_RET_ERRNO`,
  so glibc's fallback costs no stop. Today they arrive as `SIGSYS` and cost a
  ptrace round-trip each (the 263 above).
- Deep: `dn-init` owns process creation itself (a pre-forked idle pool), so
  spawn becomes "hand out a ready process + `dn-exec` maps the program".

Note the correction recorded here: `dn-tiny` does **not** substitute `clone` for
`clone3`. `policy_deny` has no `clone3` case; it answers `ENOSYS` by default and
**glibc** falls back to `clone` -- on the SIGSYS slow path, not the fast path.

`execve` could be emulated only because the kernel had already created the
process (by exec'ing `dn-exec`); process creation itself still needs the kernel,
so "emulating clone" means `dn-init` reusing a process it created earlier.

## Open decisions

1. `dn-exec` maps only the loader (leave program mapping to `ld.so`), or maps the
   whole program kernel-style? (Leaning: loader only.)
2. Exec errno: the gate decides before rewriting (synchronous), or post-exec
   failures become exit 127?
3. Context handoff: `DN_*` env holding fd numbers, or a single struct fd?
4. libc `svc` patching: keep it in the shim, or move it to `dn-exec` at map time?
5. Clone: level one (filter `RET_ERRNO`) or level two (process pool)?
6. Where `dn-exec`/shim live at a high targetSdk (APK lib vs fd), and 16 KB pages.

## Non-goals

- Not a full kernel emulation: only exec, path, identity, and `/proc`.
- Not GUI/device/emulation work.
- Not running systemd: `dn-init` is a minimal PID 1 (reap, sessions, signals),
  not a unit/journal manager.

## References

- `loader-poc.md` -- the `dn-exec` POC and its L0-L6 stages.
- `mapped-kernel.md`, `phase2.md`, `contract.md` -- ideas three and four.
- `docs/performance.md` -- the stop attribution this builds on.
- `docs/reference/feature-inventory.md` -- the feature inventory.
- History: `dn-glibc-android.patch` (retired in `2a69cba`), `0f09582` (stock
  libc6 runs; the patch was not load-bearing).
