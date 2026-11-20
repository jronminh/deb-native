# What is stable: the syscall ABI and the instruction set

deb-native keys the tracer policy by the stable Linux AArch64 syscall ABI.
Instruction-site matching depends on the changing code emitted for glibc.
[FOA's stability reference](https://github.com/jronminh/fhs-on-android/blob/main/reference/stability.md) covers both contracts.

## What it means for the shim

- **Lean on the ABI and the ISA** (stable):
  - **interpose the C symbols** (`open`, `stat`, ...) — the current symbol path;
    it manipulates no instruction, and it is fine for now.
  - **seccomp by number** (the ABI) together with the **gate marking by
    `instruction_pointer`** (the ISA) — E13.
- **Treat `svc`-site patching as a fallback** and know its floor: it depends on
  glibc's code layout, so it is version-fragile; the shim's own note holds that a
  mis-picked site costs a plain `svc`, no more (`lane.c`, `patch_libc`).
- **The instruction set is not what is fragile.** The syscall and the A64 base
  are permanent; the fragile part is always *some program's code* (glibc's, a
  compiler's). So a mechanism that reads *the ABI* (numbers, registers) or *the
  ISA* (the program counter) is safer than one that reads *a particular
  instruction sequence*.

In short: the **syscall ABI and the A64 base instructions are the two things we
can build on**; "does not manipulate the instruction set" (symbols) is the safe
path for now, and when an instruction-level rule is needed, prefer the one keyed
on the stable ISA (`instruction_pointer`) over one keyed on glibc's code.

## Checked: extending the shim's site set — three places to get right

Eleven rows the tracer only maps (no link/cwd cache) — `mkdirat`, `mknodat`,
`fchmodat`, `utimensat`, `inotify_add_watch`, `statfs`, `truncate`, and the
set/remove xattr calls — moved to the shim's fast path. It needs **three** things
in step, and each alone looks like "the site is not there":

1. **The scan needs a window.** glibc's `mkdir` is
   `mov x8,#34; mov x1,x0; mov x0,#-100; svc` — the `mov` and the `svc` are **three
   instructions apart**. The original scan looked only at `p[-1]` and missed it.
   `catch_site` now looks back a small window; a wrong pick only costs the hook
   reading `x8` at call time.
2. **`lane_hook` needs the cases** (else the hook passes the call back to the
   tracer).
3. **The filter's lane list (`DN_LANE_NRS`) needs the numbers**, or the shim's
   own re-issued call stops in the tracer.

With all three: `mkdir` 1000 times goes from **1005 stops to 5**; `mkdir`,
`chmod`, `statfs`, `setxattr` all return 0. The Lua build barely moves
(1666 -> 1662: it does few of these), but a `mkdir`/xattr-heavy workload (a
package install) would see it.

(An earlier attempt seemed to break libc with `SIGSEGV`; that was a **build
artifact**: once a broken shim is installed, `gcc` has it preloaded too, so `gcc`
dies and the rebuild fails **silently** — build to a scratch path with a good shim
installed.)

So the lesson holds and sharpens: the `svc` **encoding** and the patch
**operation** are stable; **which** calls to catch is a **policy** choice; but
realizing it is glibc's business — the site is found with a window and read at
runtime, and the filter must agree.

## Checked: the ABI net catches what the shim cannot reach

A program calling `syscall(SYS_openat, AT_FDCWD, "/etc/passwd", ...)` — a raw
syscall, bypassing the shim's `openat` symbol — opens the file correctly under
`dn-tiny`: the tracer's filter catches `nr 56` and maps the path (measured
`fd=3`, read `root:x:0:0:ro`). The cost is one stop. So the stable ABI net
(seccomp by number) covers the calls the shim cannot reach — the raw `syscall()`
path, and anything without a symbol — with no instruction-level trick. This is
the design's floor: correctness from the ABI, speed from answering in-process
where a symbol exists.
