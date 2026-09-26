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

## Experiment log

| date | experiment | result | conclusion |
|---|---|---|---|
| 2026-09-26 | full corpus scan (258 pkgs → 668 ELFs) | 7 `ET_EXEC`, 246 PIE, 415 `.so`; 6 emit `svc`; 12 import `syscall` | counts small; the structural problem is routing (`abbtr`) |
| — | Q1: in-scope programs only | *pending* | — |
| — | Q2: `syscall()` interposer | *pending* | — |
| — | Q3: `proot` overhead | *pending* | — |

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
