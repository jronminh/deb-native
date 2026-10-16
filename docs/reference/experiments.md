# Experiments: the runtime, measured

A log of small experiments on one device, with the method, so a number can be
re-checked and a claim can be told from a guess. It feeds [`TODO.md`](../../TODO.md)
and the reference pages ([`inline-hooking.md`](inline-hooking.md),
[`prior-art.md`](prior-art.md), [`device-census.md`](device-census.md)).

## Contents

- [Context](#context)
- [E1: the cost of a stop, and the filter's tax](#e1-the-cost-of-a-stop-and-the-filters-tax)
- [E2: the kernel's stop paths available here](#e2-the-kernels-stop-paths-available-here)
- [E3: execmem and execmod](#e3-execmem-and-execmod)
- [E4: where the loader's stops are](#e4-where-the-loaders-stops-are)
- [E5: patching the loader — sites and reach](#e5-patching-the-loader--sites-and-reach)
- [E6: is there a hook early enough?](#e6-is-there-a-hook-early-enough)
- [E7: patching the loader, a proof of concept](#e7-patching-the-loader-a-proof-of-concept)
- [E8: the Lua build's stops, by syscall](#e8-the-lua-builds-stops-by-syscall)
- [E9: locating the cancellable path](#e9-locating-the-cancellable-path)
- [E10: the cancellable-svc patch, tried and rejected](#e10-the-cancellable-svc-patch-tried-and-rejected)
- [What follows](#what-follows)

## Context

Samsung SM-G970W (S10e), Android 12, kernel 4.14.190, Termux in
`untrusted_app_27` (targetSdk 28). The prefix runs are in `~/work/p-dev`, a
Debian prefix whose shim is `usr/lib/deb-native/dn-lane.so` (loaded through the
exec gate's `ld.so --preload`). The runtime is `build/dn-tiny` from 2026-10-16
(the number-first filter). Timing on this phone varies 10-20% between runs;
each number below is a range over interleaved runs unless said otherwise.

## E1: the cost of a stop, and the filter's tax

Two freestanding aarch64 programs (`src` in the work tree, not in the repo yet):
`bench` loops `getpid` (a number in no row, stopped for no one) and `stopbench`
loops `mkdirat` on a missing path (number 34, in the rows: one entry stop, a
mapped path, `ENOENT`).

| what | calls | time | per call |
| --- | --- | --- | --- |
| `getpid`, native (no tracer) | 1,000,000 | 265-275 ms | ~270 ns |
| `getpid`, under `dn-tiny` | 1,000,000 | 345-367 ms | ~350 ns |
| `mkdirat`, `dn-tiny -s 500` | 200,000 | 3.5-4.2 s | **~18-21 us** |
| `mkdirat`, `dn-tiny` (sleeping) | 200,000 | 27.8-32.5 s | **~140-160 us** |

So the seccomp filter's tax on every call is about **80 ns** (350 minus 270), and
one stop costs **~19 us** polling and **~150 us** sleeping — a stop is 250-2000
times the filter's tax, which is why the number of stops is the thing to reduce.
The stop figure includes the path mapping and the real syscall, not just the
context switch.

## E2: the kernel's stop paths available here

- **`SECCOMP_RET_USER_NOTIF`** (a supervisor on a listener fd, kernel 5.0+):
  `seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, prog)`
  returns **`EINVAL`** on kernel 4.14. The plain filter (no flag) returns 0, so
  seccomp itself works. The Android 16 phone is kernel 5.10, at or above the
  5.0 floor, so it should have `USER_NOTIF`; untested there.
- **Syscall User Dispatch** (kernel 5.11+): `prctl(PR_SET_SYSCALL_USER_DISPATCH)`
  returns **`EINVAL`** on 4.14. Neither phone has it (5.10 is below 5.11).

So on the S10e the only stop paths are ptrace (`SECCOMP_RET_TRACE`, what
`dn-tiny` uses) and `SIGSYS` (`SECCOMP_RET_TRAP`, what Android's filter does);
`USER_NOTIF` can only be tried on the S23 FE (kernel 5.10).

## E3: execmem and execmod

In the Termux domain (`untrusted_app_27`, targetSdk 28), by raw `mprotect`:

| transition | result |
| --- | --- |
| anonymous `RW` -> `mprotect RX` (execmem) | OK |
| anonymous `RWX` mmap directly | OK |
| libc text -> `RWX` (execmod) | OK, restorable to `RX` |
| libc text -> `RW` (execmod) | OK, restorable to `RX` |

So both the anonymous trampoline page and the in-place patch of a library's text
are permitted here: the shim's technique works in this domain. The open question
is a targetSdk 29+ app (`TODO.md`, "A prefix in any app").

Method note: the transition must be done with a raw syscall and restored before
any libc call, or the page holding the running code loses its `X` and the process
faults. The first attempt here did exactly that and crashed; the working test
toggles the text page with `svc` and calls libc only after restoring `RX`.

## E4: where the loader's stops are

`dn-tiny -t ROOT -- PROG` directly (no `init.sh`), stops counted from its own
line, the shim on and off (the shim off = its file renamed aside):

| program | shim on | shim off | saved |
| --- | --- | --- | --- |
| `true` | 5 | 4 | -1 |
| `gcc --version` | 5 | 5 | 0 |
| `sed -n 1p` | 12 | - | - |
| `git --version` | 9 | 33 | 24 |
| `python3 -c pass` | 15 | 108 | 93 |
| `dpkg -l` | 83 | 91 | 8 |

Reading: every process pays a **startup floor of ~4-9 stops** for the loader
opening the program, the shim and the libraries — all **before the shim's
constructor runs**, so the shim answers none of them (it only adds one, for its
own load: `true` 5 vs 4). The floor grows with the number of linked libraries
(`git` 9, `true`/`gcc` 5). A late `dlopen` also goes through the loader:
`python3 -c 'ctypes.CDLL("libm.so.6")'` is 2 stops over `python3 -c pass`.

So the `ld.so` count in the Lua build (1082, `performance.md`) is this
per-process floor summed over the build's many execs (gcc spawns cc1, as, ld),
not one process.

## E5: patching the loader — sites and reach

The same pattern the shim patches in libc (`mov x8,#nr; svc`, `lane.c`) exists
in the loader. Counting them in the files of the `p-dev` prefix:

| object | `svc` sites (all) | path sites of interest |
| --- | --- | --- |
| `ld-linux-aarch64.so.1` | 40 | `openat` x3, `newfstatat` x1, `fstat` x1 |
| `libc.so.6` | 325 | `openat` x2, `faccessat` x1, `newfstatat` x4, `statx` x1, `readlinkat` x2, `fstat` x2, `getcwd` x1, `execve` x1, `unlinkat` x3, `chdir` x1 |

The loader carries its own copies of the opens (it is self-contained), so they
are patchable the same way. The ±128 MB question (a `B`'s reach) is answered at
run time, with the shim's own near-memory step: a stub page within range is
available — ld.so text at `0x7e6645c000`, an `mmap` at `0x7e66600000` (+1680 KB);
libc text at `0x7e661a0000`, an `mmap` at `0x7e66490000` (+3008 KB). Both in
reach.

So the mechanics are fine; the blocker is **when** a patch runs (E6).

## E6: is there a hook early enough?

The shim is a preloaded object: its constructor runs after the loader has opened
everything (E4). A hook that runs earlier is `LD_AUDIT`, whose module the loader
loads and calls back during loading. A freestanding audit module (no libc, or it
would pull libc in first) that prints the lines of `/proc/self/maps`:

```
audit: la_version   maps lines=14  libc=0  shim=0  program=1
audit: la_preinit   maps lines=27  libc=1  shim=1  program=1
```

At `la_version`, **libc and the shim are not yet mapped**; the loader calls it
before it opens them. So a freestanding `LD_AUDIT` module could patch the
loader's `openat`/`newfstatat` sites at `la_version`, before the loader makes
those opens — the per-process startup stops the shim cannot reach. The cost is
the module's own load (one open, one stop) against the several it could save.

## E7: patching the loader, a proof of concept

A freestanding `LD_AUDIT` module (`ldhook.c` in the work tree, not in the repo).
At `la_version` it finds ld.so's `r-xp` segment in `/proc/self/maps`, scans it
for `mov x8,#56; svc` (its three `openat` sites), maps a stub page within a
`B`'s reach, and replaces each `svc` with a `B` to a stub that sets the lane
cookie in `x5`, re-issues the `svc`, and branches back to the instruction after
the original `svc`. The tracer's filter sees the cookie and lets the call
through with no stop.

The loader's paths are **host paths** — the exec gate passes the program and the
library path as host paths (`exec.c`) — so the stub needs no path law: it only
adds the cookie. Stops, `dn-tiny -t ROOT -- PROG`, the shim present:

| program | before | after |
| --- | --- | --- |
| `true` | 5 | 4 |
| `git --version` | 9 | 6 |
| `python3 -c pass` | 15 | 11 |

The net is the loader's opens removed (two for `true`, four to five for `git`)
minus one for loading the module itself. Patching the `newfstatat` and `fstat`
sites as well changed nothing on these three small programs.

**On a real build** (the Lua 5.4.7 build, `make -j1 linux`, 152-159 execs, three
interleaved pairs):

| | stops | wall |
| --- | --- | --- |
| before | 1666 (every run) | 11.41-11.49 s |
| after | 834 (every run) | 10.62-11.01 s |

The loader's stops fall by exactly 832 — its startup floor (E4) removed for
every one of the build's execs — and the wall time drops ~5%. The stop reduction
is deterministic; the wall gain is smaller than 832 stops at ~19 us would
explain, so part of it is the tracer's per-stop work (path mapping, the context
switches) rather than the stop itself. This is the "many small gains" case: each
exec saves a few stops, and a build has hundreds of execs.

Two bugs were found and fixed on the way, both worth remembering:

- The stub's back-branch was written as `*stub++ = b_enc(stub, ...)`; the
  `stub` inside the expression is the **post-increment** value, so the branch
  was four bytes short and landed on the `svc` site itself — now holding the
  `B` — an infinite loop. Fixed with a named temporary for the origin.
- The cookie's high two 16-bit halves were swapped in the `movz`/`movk`
  sequence (`0x9e1ab6c3d7f2a541` needs `movk hw=2 = 0xb6c3, hw=3 = 0x9e1a`), so
  the filter never matched and the calls still stopped.

The module is a proof of concept: it assumes the loader's paths are host paths
and that each site is entered with `x8` already the number, and it has no
fallback. It runs in `~/work/p-dev`; the source is not committed.

## E8: the Lua build's stops, by syscall

A temporary counter in the tracer (the syscall number at each entry stop; the
`mon_read` the tracer already has, to name the path), then reverted. The Lua
build, 1666 stops:

| syscall | stops | |
| --- | --- | --- |
| `openat` (56) | 1265 | 1146 read, 119 write |
| `execve` (221) | 159 | one per exec, the gate |
| `faccessat` (48) | 127 | |
| `unlinkat` (35) | 79 | writes |
| rest | 36 | `getresuid`/`getresgid`, `chdir`, `statfs`, `fchmodat`, `mkdirat` |

With E7 (the loader patched, 834 stops): `openat` 452 (341 read, 111 write),
`execve` 152, `faccessat` 121, `unlinkat` 75.

What the reads are, from the paths (a `open` read stop named its path):

- **The loader's opens** — the program, the shim, each library, `cc1`, `as`,
  `ld`, as **host paths**. E7 removes ~805 of them.
- **The program's own opens** — as **guest paths** (`/etc/hostname`). These split
  by how the call is made: `open()` (the symbol) is interposed by the shim and
  answered in-process, **no stop**; `fopen()` (glibc's internal cancellable path)
  is **not** patched by the shim, so it **stops**. A minimal `open()` program
  shows no stop for its file; a minimal `fopen()` program shows one.

So the ~341 program read stops left after E7 are the shim's unpatched
**cancellable path** — the number `performance.md` gives (324). The shim leaves
it because `pthread_cancel` reads the program counter there.

**The ceiling:** of `openat`, only the writes (119) must stop (they change the
tree); the rest are reads, answerable in-process. So `openat` could go from 1265
to ~119.

**Improvement points (fewer stops):**

1. **The cancellable-path stub** (`TODO.md`, "libc's cancellable path"), ~324-341
   stops a build. Located in E9; its stub must keep the program counter in the
   cancellation range. Not a one-line change.
2. E7 (the loader) — done.
3. `faccessat` (127) is the same libc-internal family.

## E9: locating the cancellable path

Disassembled the prefix's `libc.so.6` (`llvm-objdump -d`). The cancellable
wrappers set the number in **x6** and branch to one function:

| symbol | does |
| --- | --- |
| `open64` (0xe3540) | `mov x6, #56`, args, `bl 0x828ec` |
| `read` (0xe3e00) | `mov x6, #63`, args, `b 0x828ec` |
| `write` (0xe48e0) | `mov x6, #64`, ... |

`0x828ec` is `__syscall_cancel_arch`; inside, at `0x82868`, `mov x8, x6`
(`0xaa0603e8`), and a single `svc #0` at `0x828d8` serves **every** cancellable
syscall. So the shim's adjacent `mov x8,#nr; svc` pattern cannot match it (the
`mov` is from `x6`, and instructions sit in between), and one patch of that one
`svc` would cover all cancellable calls — `lane_hook` already reads `x8`.

Locating it at run time is hard: `mov x8,x6` appears ten times in libc, and the
function is not an exported symbol. So it is found at **build time**, from the
known libc: [`scripts/find-cancellable.py`](../../scripts/find-cancellable.py)
looks for the one `mov x8,x6` with an `svc` within 30 instructions (on this
prefix's libc 2.41 it prints `offset 0x828d8`; on `ld.so` it reports none). The
offset can be handed to the shim instead of a runtime scan.

The remaining open problem is the passthrough: for a call the shim does not
answer, the stub's own `svc` runs with the program counter outside the
cancellation range, which is what `pthread_cancel` reads.

## E10: the cancellable-svc patch, tried and rejected

The fix E9 points at: patch the loader's cancellable `svc` as well, found at run
time by the `mov x8,x6` + `svc`-within-30 heuristic (the shim already reads `x8`
at call time, so the same stub works).

It does work. The Lua build's stops fall **1666 -> 1463 (-203)** and the wall
~14.25 -> ~14.15 s (interleaved); a minimal `fopen` program loses one stop
(`op2` 6->5, `sed` 12->11); and a `pthread_cancel` test still cancels
(`cancelled=1`). It is **not shipped**, for three reasons:

- The site is glibc's **internal** `__syscall_cancel_arch` (the number in `x6`),
  with no stable contract. A heuristic that mis-identifies it could patch the
  wrong `svc`; the shim's other sites degrade to a plain `svc` when picked
  wrongly, but a shared generic one does not.
- That `svc` is shared, so every cancellable call (`read`, `write`, ...) now runs
  through `lane_hook`, a small tax on the calls it does not answer.
- The win is ~1% of one build.

`scripts/find-cancellable.py` keeps the build-time way (it fails loudly when the
libc does not match) if the ~1% is ever wanted; the run-time heuristic is
reverted. The decision sits in `TODO.md`, "libc's cancellable path".

## What follows

The loader's startup stops can be removed by a freestanding audit module that
patches the loader (E7). Left before this is real: patch the `newfstatat` and
`fstat` sites too; decide whether the program's own opens (not only the loader's)
can take the same lane, which would need the path law in the module; and measure
a full build (the Lua build's 1082 loader stops) rather than a single program.

Until then, the startup stops are simply paid; they are small per process and
become visible only across many execs.
