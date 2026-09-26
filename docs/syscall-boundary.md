# The syscall boundary — what libc interposition cannot see

The shim ([`shim-coverage.md`](shim-coverage.md)) rewrites paths at the
**preemptible dynamic symbol** layer. Everything that reaches the filesystem
without crossing such a symbol is out of its reach. This doc maps that wider
boundary, measures it against the in-scope corpus, and records where Termux's
existing answer (`proot`) already applies.

## The cases, not just "static vs dynamic"

| # | case | example | shim sees it? | mechanism |
|---|---|---|---|---|
| 1 | ordinary libc path call | `open("/etc/x")` | **yes** | shim |
| 2 | libc-internal / statically-bound call | NSS `__open_nocancel`, loader internals | no | tracer |
| 3 | explicit `syscall()` wrapper | `syscall(SYS_openat, …)` | no, but *could* | add a `syscall` interposer |
| 4 | inline `svc #0` | Go, Rust, static-musl, inline asm | no | tracer |
| 5 | fully static executable (no `PT_INTERP`) | `bash-static`, `busybox` | no (no dynamic linker) | `proot` / tracer |

Cases 2 and 4 are the ones that read as "static libc symbols": the code — in
libc or in the binary — issues the call directly, not through the PLT. Case 3
is different and is the interesting one: `syscall()` is a **public** libc
symbol, so a program that calls it *is* interposable, and the shim could gain
one function instead of needing a tracer. Nothing in the current shim does.

## Measured against the corpus

Corpus: the 258 in-scope packages from [`shim-coverage.md`](shim-coverage.md),
extracted to 668 ELFs.

- **7 `ET_EXEC`** (non-PIE executables), **246 PIE executables** (dynamic),
  **415 shared objects**.
- **6 binaries emit `svc #0` in their own code** (objdump-verified):
  `busybox` (249), `bash-static` (188), `abbtr` (38), and the Go tools
  `arduino-builder`/`balloon`/`c2go` (37 each).
- **12 binaries import a `syscall` symbol** (case 3 — the syscall is in libc).
- Go-marked: 3; Rust-marked: 4.

The count is small, but the point is structural, not numerical.

## The gap is routing, not the count

`native/dn-run.c` classifies a binary by `PT_INTERP` and routes it: glibc →
the shim, static → `proot`. **`abbtr` is a PIE (dynamic) executable that emits
`svc #0` itself**, so it is routed to the shim — which cannot see any of its 38
syscalls. "Has a dynamic interpreter" is not the same as "all its filesystem
access goes through libc". A `PT_INTERP` check is necessary but not sufficient;
a binary needs a **direct-syscall attribute** of its own.

## Termux's existing answer

`proot` — a `ptrace` syscall interceptor, already shipped and already wired as
`dn-run`'s static route (`proot -b host:guest …`). It works at the syscall
layer, so it covers cases 2–5 uniformly, which is why the project reaches for
it instead of building a tracer first. Two caveats, both already visible:
`ptrace` overhead on every syscall, and `proot`'s bind model errors on a
missing host path (`dn-run.c`'s comment).

## Solved (2026-09-26): NSS, case 2

Measured on `fe2`: the missing piece was not the syscall layer but the *path*.
`PROOT_VERBOSE=2` shows Termux's glibc reads its **sysconfdir**
`$PREFIX/glibc/etc/passwd` — a host path outside the prefix — not the guest
`/etc/passwd`. So neither the LD_PRELOAD shim nor a plain `/etc` bind reaches
it, which is why `getpwnam` returned `NOTFOUND` even under `proot`.

Fix: on the NSS route, bind the prefix's `/etc` over Termux glibc's sysconfdir
(`-b $INSTDIR/etc:$PREFIX/glibc/etc`). Then `getpwnam("dnshim")` resolves in
the prefix. `native/dn-run.c` now gives a glibc ELF an **NSS-import attribute**
(scan for `getpwnam`/`getpwuid`/`getaddrinfo`/… in the binary) and routes it
through the tracer (`dn-trace`, else Termux `proot`) with that bind; static
binaries take the tracer route too. Verified by `tests/tracer-nss/run.sh`
(PASS). glibc still synthesizes `root`/`nobody`/Android uids; the bind only
makes the prefix's `passwd`, `group`, `hosts`, `resolv.conf` authoritative.

## Method

`scripts/scan-direct-syscalls.py` reports the cases above: it parses the ELF,
scans **only** executable sections at 4-byte alignment for `svc #0` as a
candidate filter, and disassembles each candidate with `objdump` to remove
literal-pool false positives. The warning that motivated it: a whole-file byte
grep for `svc #0` flags **111** binaries; disassembly confirms **6**. Data is
not code.

```
python3 scripts/scan-direct-syscalls.py DIR --verify --list
```

## Options, cheapest first

1. **Extend the `proot` route** from "static" to "any binary with direct
   syscalls" (inline `svc` or a `syscall` import). Small change in
   `dn-run.c`'s classifier, uses the tool already trusted; cost is `ptrace`
   overhead on those binaries only.
2. **Interpose `syscall()`** in the shim for case 3. Cheap, no `ptrace`, but
   only the wrapper — not inline `svc`.
3. **A `SECCOMP_RET_USER_NOTIF` / `ptrace` tracer** — the endgame; covers
   2–5 and lets the shim stay fast for case 1. Platform probes say it is
   feasible here (`ptrace` works in the app uid).

## Open questions

These are tracked, with an experiment log and the mechanism decision, in
[`direct-usage.md`](direct-usage.md).

- The corpus is dominated by **libraries** (415 of 668). What matters is the
  in-scope **programs** on `PATH`; that breakdown is not yet done.
- Does interposing `syscall()` actually catch the 12 importers, and at what
  cost? (Cheap to test with the existing `tests/shim-libc` harness.)
- What is `proot`'s real overhead on a package's hot paths, measured, before
  choosing option 1 over 3?
