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

## What follows

The pieces line up for removing the loader's startup stops: patchable sites
(E5), in-range stubs (E5), and a hook early enough (E6). What is missing is
engineering: a freestanding audit module that carries the path law (a build of
`policy.c` without libc, the "one copy of the law" in `TODO.md`) and answers the
loader's opens in-process. E7 is a proof of concept: patch one `openat` site in
the loader and measure the stop count.

Until then, the startup stops are simply paid; they are small per process and
become visible only across many execs.
