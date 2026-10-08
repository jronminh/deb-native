# The syscall boundary — what libc interposition cannot see

<!-- template: templates/docs.template.md -->

The shim ([`shim-coverage.md`](../spec/overlay.md)) rewrites paths at the
**preemptible dynamic symbol** layer. Everything that reaches the filesystem
without crossing such a symbol is out of its reach. This doc maps that wider
boundary, measures it against the in-scope corpus, and records where the
existing answer (`proot`) already applies.

## Contents

- [The cases, not just "static vs dynamic"](#the-cases-not-just-static-vs-dynamic)
- [Measured against the corpus](#measured-against-the-corpus)
- [The gap is routing, not the count](#the-gap-is-routing-not-the-count)
- [The existing answer](#the-existing-answer)
- [Solved: NSS, case 2](#solved-nss-case-2)
- [Status by case (2026-09-26)](#status-by-case-2026-09-26)
- [Solved (2026-09-26): the direct-syscall attribute (cases 3/4)](#solved-2026-09-26-the-direct-syscall-attribute-cases-34)
- [Options, cheapest first](#options-cheapest-first)
- [Open questions](#open-questions)

## Related docs

- [`shim-coverage.md`](../spec/overlay.md) — the shim this doc's boundary
  sits beyond.
- `direct-usage.md` — the living investigation into
  what actually crosses this boundary.
- [`tracer.md`](../spec/overlay.md) — the mechanism that reaches past it.
- [`android-platform.md`](android-platform.md) — the Android-specific
  limits (seccomp/capability/SELinux) layered on top of this boundary.

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

Corpus: the 258 in-scope packages from [`shim-coverage.md`](../spec/overlay.md),
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

`src/dn-run.c` classifies a binary by `PT_INTERP` and routes it: glibc →
the shim, static → `proot`. **`abbtr` is a PIE (dynamic) executable that emits
`svc #0` itself**, so it is routed to the shim — which cannot see any of its 38
syscalls. "Has a dynamic interpreter" is not the same as "all its filesystem
access goes through libc". A `PT_INTERP` check is necessary but not sufficient;
a binary needs a **direct-syscall attribute** of its own.

## The existing answer

`proot` — a `ptrace` syscall interceptor — is the prior art here. It works at
the syscall layer, so it covers cases 2–5 uniformly. The project builds its own
reduced tracer (`dn-trace`, [`../spec/overlay.md`](../spec/overlay.md))
instead, because `ptrace` overhead on every syscall is the cost to contain, and
`proot`'s bind model errors on a missing host path.

## Solved: NSS, case 2

NSS reads are libc-internal, so neither the shim nor a plain path map reaches
them. The prefix's own glibc derives its sysconfdir from the live prefix, so
`getpwnam`/`getgrgid`/… resolve against the prefix's `/etc` (its `passwd`,
`group`, `hosts`, `resolv.conf`). `src/dn-run.c` gives a glibc ELF an
**NSS-import attribute** (scan for `getpwnam`/`getpwuid`/`getaddrinfo`/… in the
binary) and routes it through the tracer; static binaries take the tracer route
too. Verified by `tests/tracer-nss/run.sh`.

## Status by case (2026-09-26)

| # | case | mechanism | status |
|---|---|---|---|
| 1 | libc path call | shim | done |
| 2 | libc-internal / NSS | tracer + glibc-sysconfdir bind | **solved** (`tests/tracer-nss`) |
| 3 | explicit `syscall()` import | tracer | done |
| 4 | inline `svc #0` | tracer | done |
| 5 | fully static exe | tracer (no `PT_INTERP` → `C_STATIC`) | done |

Before 2026-09-26, cases 3 and 4 failed **silently**: a PIE such as `abbtr`
has `PT_INTERP`, so `dn-run` classified it `C_GLIBC` and sent it to the shim —
but its syscalls are issued in its own code, which the shim never sees, so it
ran with no redirection at all (host `/etc`, `/usr`, …).

## Solved (2026-09-26): the direct-syscall attribute (cases 3/4)

A binary that issues its own syscalls is routed to the tracer, same as NSS.
The install-time scan finds the ELFs that need it — a `syscall` symbol import
(case 3) or an `objdump`-verified `svc #0` (case 4) — and those programs run
`dn-run --trace`, forced to the tracer route. Cost is tracer overhead on those
binaries only; the shim stays fast for the rest.

Detection is disassembly, not a byte search: a whole-file grep for `svc #0`
flags 111 binaries; disassembly confirms 6. It parses each ELF, scans **only**
executable sections at 4-byte alignment for `svc #0` as a candidate filter,
and disassembles each candidate with `objdump` to remove literal-pool false
positives. Data is not code.

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
`direct-usage.md`.

- The corpus is dominated by **libraries** (415 of 668). What matters is the
  in-scope **programs** on `PATH`; that breakdown is not yet done.
- Does interposing `syscall()` actually catch the 12 importers, and at what
  cost? (Cheap to test with the existing `tests/shim-libc` harness.)
- What is `proot`'s real overhead on a package's hot paths, measured, before
  choosing option 1 over 3?
