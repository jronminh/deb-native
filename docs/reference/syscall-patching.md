# Patching a syscall site (AArch64)

How the runtime turns a libc or loader syscall into an in-process call, at the
instruction level. The code is `src/dn-tiny/lane.c` (`patch_libc`, the shim) and
the branch's `ldhook.c` (the loader, E7). This is the mechanics only; the filter
and the lane are in [`architecture.md`](../architecture.md) and `lane.h`.

## The site

A caught call is a **`svc #0`** preceded by **`mov x8, #nr`**:

- `svc #0` = `0xd4000001`.
- `movz x8, #imm16` = `0xd2800008 | (imm16 << 5)`. Tested as
  `(w & 0xffe0001f) == 0xd2800008`: the mask keeps the opcode and `Rd`, drops
  `imm16`. The number is `(w >> 5) & 0xffff`.

The `mov x8` is **not always the instruction before the `svc`**: glibc's `mkdir`
is `mov x8,#34; mov x1,x0; mov x0,#-100; svc`. So look back a **window** (the
shim: 8 instructions) for a `movz x8`. A wrong pick costs nothing but a plain
`svc` (the hook reads `x8` at call time and passes through if the number is not
one it answers).

Only the numbers the runtime answers are patched (`hooked_nr`, `lane.h`); the
**cancellable path** (the number in a register, `x6`, not `x8`) is left alone --
`pthread_cancel` checks the program counter there (E9-E10).

## The patch

Replace the `svc` with a **branch to a stub** for that site:

```
site:  ... mov x8,#nr          site+4: <the instruction after the svc>
       svc #0        ->   b <stub>          (0x14000000 | imm26)
```

`b`/`bl` reach **±128 MB** (`imm26` in instructions); the runtime treats it as
±120 MB (`in_reach`). The stubs go in a page `mmap`ed **near the text** (a hint
of `text_end + k*2 MB`, 1 MB-aligned, tried `k = 1..8`) so every site is in
reach.

## The stub and the shared block

Each site gets a small stub; all stubs call one shared block (`emit_common`):

```
stub(site):                            common:
  sub  sp, sp, #272                      save x0..x27 (stp/ldp), x28, x30
  stp  x29, x30, [sp,#232]               mrs  x9, nzcv; str x9, [sp,#248]
  bl   common                            mov  x0, sp          # the saved frame
  ldp  x29, x30, [sp,#232]               ldr  x16, =lane_hook_entry  # a literal
  add  sp, sp, #272                      blr  x16
  b    site+4            # back          msr  nzcv, x9; ldr x30
                                         cbz  x0, passthrough  # hook said 0
                                         ldr  x0/x1, ...       # the answered values
                                         restore x2..x28; ret
                                       passthrough:
                                         restore x0..x28
                                         svc  #0; ret          # the original call
```

So a site whose number the hook does **not** answer falls through to the real
`svc`; one it answers returns from the stub with `x0` (and `x1`) set. `x30` is
saved because `blr` overwrites it. The stubs run on the hook's **own 128 KB
per-thread stack** (`lane_on_stack`), so a deep guest stack cannot overflow it
(the `py3compile` crash).

## The instruction words used

| what | encoding | word |
| --- | --- | --- |
| `svc #0` | `0xd4000001` | |
| `movz x8,#i` | `0xd2800008 | (i<<5)`; mask `0xffe0001f` | |
| `b imm26` | `0x14000000 | (imm26)` | |
| `bl imm26` | `0x94000000 | (imm26)` | |
| `ret` | `0xd65f03c0` | |
| `nop` | `0xd503201f` | |
| `stp xA,xB,[sp,#off]` | `0xa9000000 | (off/8)<<15 | B<<10 | 31<<5 | A` | |
| `ldp` | `0xa9400000 | ...` | |
| `str xT,[sp,#off]` | `0xf9000000 | (off/8)<<10 | 31<<5 | T` | |
| `ldr` | `0xf9400000 | ...` | |
| `mrs x9,nzcv` / `msr nzcv,x9` | `0xd53b4209` / `0xd51b4209` | |
| `mov x0,sp` | `0x910003e0` | |
| `blr x16` | `0xd63f0200` | |
| `ldr x16,=lit` (PC-rel) | `0x58000000 | (imm19<<5) | 16` | |
| `cbz x0,off` | `0xb4000000 | (imm19<<5)` | |
| `sub/add sp,sp,#272` | `0xd10443ff` / `0x910443ff` | |
| `movn x0,#0` | `0x92800000` | |
| `movz/movk x5` (the cookie) | `0xd2800005 | ...` / `0xf2800005 | (hw<<21) | (imm<<5)` | |

The address of `lane_hook_entry` is a **literal in the code** (`ldr x16, =addr`),
8-aligned (a `nop` pads if needed). The cookie
`0x9e1ab6c3d7f2a541` is `movz x5,#0xa541` then `movk hw=1 #0xd7f2`, `hw=2
#0xb6c3`, `hw=3 #0x9e1a` (the halves were swapped once -- E7).

Every instruction here is a **fixed 4 bytes** (A64 has no variable width), so a
site is one word and a stub is a whole number of words.

## Cache and permissions

Writing code needs both, in this order (`flush`):

1. `dsb ish; dc cvau, <line> (each 64 B); dsb ish; ic ivau, <line>; dsb ish; isb`
   -- `__builtin___clear_cache` emits this. Without the `ic ivau` the core runs
   the stale instruction.
2. **`execmod`**: the text page is made writable. The shim calls
   **`mprotect` by raw `svc`** (`plain_svc`), not libc's -- libc's `mprotect`
   may itself sit in the text just made writable. `RWX` first, then plain `RW`,
   then patched, then restored to `RX`. On `untrusted_app_27` this is allowed
   (E3); a targetSdk 29+ app is the open question (`TODO.md`, "A prefix in any
   app").

## The minimal variant (the loader)

When E7 was measured the loader's paths were **host paths** (the exec gate passed
host paths then), so its stub needed no path law and no hook call -- only the
cookie (E7):

```
stub: movz x5,#0xa541; movk hw=1..3 ...; svc #0; b site+4
```

Six words, no register save, no separate block. The loader's text is found from
`/proc/self/maps` (`LD_AUDIT` runs at `la_version`, before `dl_iterate_phdr`
would know libc), and the stub page is mapped within `B` reach the same way.

**But this is no longer the loader's case.** The exec gate now hands the program
and `--library-path` as the tree's own paths (Phase 0 step 2), so *every* path the
loader opens is a guest path and the cookie-only stub is wrong for all of them:
the stub must map each through the one law (`policy.c`), the same as the shim.
The loader had already shown the shape of this with its `faccessat` on
`/etc/ld.so.preload` and its `dlopen`s of plugins.

## What can go wrong

- **A jump lands between `mov x8` and `svc`**: the patch assumes the pair is
  straight-line. gVisor (#13362) and ASC-Hook raise the same. Check the sites.
- **A wrong site pick**: harmless -- the hook reads `x8` and passes through.
- **Reach**: a site outside ±120 MB of the stub page is skipped.
- **`x8` not the number at entry**: the minimal (loader) stub assumes it is; the
  full hook reads it, so it is safe there.
- **Cancellable path**: not patched (the number is in `x6`).
- **Concurrency**: a site is patched while other threads may run it -- the
  write is one aligned word (atomic on A64) plus the cache maintenance.
