# Performance

What the runtime costs, measured on one phone, and where the time goes. The
mechanism is in [`architecture.md`](architecture.md).

All figures: Samsung Galaxy S10e (SM-G970W), Android 12, kernel 4.14, on battery,
October 2026. "Termux" means run from a Termux shell; "app" means from inside a
session of an app (dn-shell, `untrusted_app_27`, targetSdk 28). Runs of the same
configuration differ by 10-20%; comparisons were interleaved.

## Contents

- [The cost of a stop](#the-cost-of-a-stop)
- [In an app session](#in-an-app-session)
- [How the stops came down](#how-the-stops-came-down)
- [What is left](#what-is-left)
- [A second bench](#a-second-bench)
- [How to measure](#how-to-measure)

## The cost of a stop

A syscall the filter lets through costs what it costs natively (`stat`: about
0.8 us). One the tracer handles costs a stop: 120-600 us when the tracer sleeps
between stops, about 30 us with `-s 500`. The floor is the kernel, not the
tracer's work: the tracer's core sits at a low clock or in an idle state, and
every stop pays to wake it and to switch processes. The same call answered in the
shim costs about 1 us.

So what matters is the number of stops, and the runtime is built to have few.

A call that stops for no one still runs the filter. On the S10e (kernel 4.14),
a million `getpid` (in no row, not in the lane): 465 ms with the filter that
scanned the rows in order, 355 ms with one that dispatches on the number and
tests the rows as a tree, and 317 ms with the lane folded into that same tree --
the lane list is no longer scanned first, so a call in no row and no lane is a
few comparisons from `ALLOW` (interleaved, the last step is about 57 ns a call).
A file-tree subset (`find`) moves with it. The kernel's per-number filter cache
(5.11 and later) is not on this device, so these are instruction counts, not
cache hits.

## In an app session

The app's earlier runtime (dn-tiny without the shim, 2026-10-14) against the
current one, in the same app session, interleaved:

| workload | before | now |
| --- | --- | --- |
| Lua 5.4.7, `make -j1` with gcc | 91.8 s (58.3k entry, 46.7k exit stops) | **11.9 s** (1720, 0) |
| a regression set (`ls -lR`, `find` over `/usr` and `/etc`, `stat`, `readlink`, Python, `tar`, `cp -a`) | 64.2 s | **0.88 s** |
| file-tree set (`find /usr`, `git status`, `dpkg -l`, `apt-cache show`, `ls -lR`) | 2.3-2.9 s | **0.27-0.36 s** |
| `find /usr -type f` (12.9k files) | 1.29-1.36 s | 59-72 ms |
| `ls -lR /usr/share/doc` | 0.68-0.99 s | 46-73 ms |

For reference, natively in Termux: `find` over the same tree 57 ms, `ls -lR` 31
ms; Termux's own clang builds the same Lua tree in 9.9 s (another compiler).
With the shim, `-s` hardly matters any more: there are too few stops left.

## How the stops came down

The Lua build, in Termux:

| step | entry stops | exit stops | time |
| --- | --- | --- | --- |
| rows taken from PRoot's tables (86 rows) | 67.6k | 50.6k | ~50 s |
| catch only what the tracer acts on (51 rows) | 58.3k | 46.6k | 46.6 s |
| answer `readlinkat`, `newfstatat`, `fstat` at entry | 58.4k | 127 | 31-43 s; 17.6-18.2 s with `-s 500` |
| the shim, `realpath` only | 20.4k | | 14.6 s (`-s 500`) |
| the shim: `open`, `stat`, `access` | 5.3k | 122 | 11.3-12.2 s (`-s 500`) |
| fake root without `fstat` stops, `statx` at entry | 3.6k | 122 | 13.0 s without `-s` |
| `euidaccess` in the shim; `C.UTF-8` link, no `rseq` | 2.8k | 122 | |
| dirfd calls, `statx`, `getcwd` in the shim | 2.6k | 0 | ~11 s |
| libc's `svc` sites patched | **1.7k** | **0** | ~11 s |

The build's time is now gcc's own work; the stops left cost about 0.1 s of it.

## What is left

The Lua build's 1.7k entry stops, by where the `svc` is:

| site | stops | |
| --- | --- | --- |
| `ld.so`, `openat` and `faccessat` | 1082 | the program, the shim and the libraries opened before the shim is loaded |
| libc's cancellable path, `openat` | 324 | not patched (`pthread_cancel` checks the program counter there) |
| `execve` | 195 | the exec gate: needed |
| `unlinkat` | 79 | writes always reach the tracer |
| the shim asking the ids | 20 | once per process that asks |

and 263 `SIGSYS` stops from the app's own filter (`set_robust_list`, `clone3`),
258 fork and exec events.

## A second bench

The Lua build is one axis: a compile, dominated by one heavy process, with a
few `exec`s. It hides three others: a workload that is nothing but `stat` and
`readlink`, one that moves links, and one that spawns many short processes.
`scripts/bench.sh PREFIX` runs such workloads, each its own run of the runtime,
so the stops attribute to one workload:

| workload | what it does | wall | entry stops |
| --- | --- | --- | --- |
| `find` | `find /usr -type f` (13k files) | ~140 ms | 61 |
| `ls` | `ls -lR /usr/share` | ~510 ms | 60 |
| `links` | `find -type l -printf`, `stat -L`, `realpath` | ~170 ms | 78 |
| `spawn` | 300 x `/usr/bin/true` | ~3.36 s | 1548 |
| `git` | `init`, `add`, `commit`, `status` over a cert tree | ~460 ms | 563 |
| `py` | `os.walk` + `stat` over `python3.13` | ~150 ms | 63 |
| `tar` | `tar czf` then `tar xzf` a cert tree | ~430 ms | 711 |

Two things it shows that the Lua build hides:

- **Metadata is cheap now.** `links` and `py` do thousands of `stat`, `readlink`
  and `lstat`, in a single process, and stop under 100 times: the shim answers
  them (a call in the shim is about 1 us, a stop about 30 us). A C loop of 2000
  `fstatat` in one process stops **5** times.
- **Every spawned process costs about 5 stops.** 300 x `true` stops 1548 times,
  against 7 for the empty loop. The cost is the exec gate plus the loader's own
  `open`s, which the shim cannot answer: they happen before it is loaded. On
  this phone natively in Termux the same 300 `true` take 2.4 s, so the runtime
  adds about 1 ms a spawn and the stops are about 0.05 s of it. Any
  process-spawning workload pays this — the Lua build's ~150 `exec`s pay it too,
  but under gcc's own work it is not seen.

## How to measure

- `scripts/bench.sh PREFIX [workload ...]` runs the second bench above.
- `dn-tiny` without `-q` prints one line at exit: entry and exit stops, events,
  signals, paths mapped, calls answered at entry, execs, `SIGSYS` answers, the
  time, and which syscalls the platform killed.
- `-m bare` stops without doing anything (the cost of a stop alone); `-c CPU`
  pins the tracer.
- In the prefix, `DN_LANE_PATCH=0` turns off the libc patching; moving
  `usr/lib/deb-native/dn-lane.so` aside turns off the shim.
- `build/dn-census` lists which syscalls the platform kills
  ([`reference/device-census.md`](reference/device-census.md)).
