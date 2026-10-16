# Inline hooking: patching instructions to intercept syscalls

A reading list on manipulating the instructions a process runs, on arm64 and
under Android. It is a separate page because it is about the CPU and the
operating system, not about the tree the runtime serves: the shim (`dn-lane.so`,
[`architecture.md`](../architecture.md), "The shim") is an inline hook, and this
page collects what is known about that technique and what each source offers.

This is a list, not a design, and not a claim that any of it is measured here.
Sources the text calls unchecked are summaries, not silicon. Checked 2026-10-16.

## Contents

- [Two axes of a syscall's cost](#two-axes-of-a-syscalls-cost)
- [The ARM architecture: the sources](#the-arm-architecture-the-sources)
- [The mechanics of a patch on arm64](#the-mechanics-of-a-patch-on-arm64)
- [Trampoline construction](#trampoline-construction)
- [The instruction cache, W^X, and Android](#the-instruction-cache-wx-and-android)
- [The memory model and the barriers](#the-memory-model-and-the-barriers)
- [Thread-safety of a live patch](#thread-safety-of-a-live-patch)
- [Dynamic binary instrumentation: where inline hooking sits](#dynamic-binary-instrumentation-where-inline-hooking-sits)
- [The kernel's own nets: seccomp and SUD](#the-kernels-own-nets-seccomp-and-sud)
- [Rewriting the syscall instruction itself](#rewriting-the-syscall-instruction-itself)
- [Engines to read](#engines-to-read)
- [What each means here](#what-each-means-here)
- [Reliability](#reliability)

## Two axes of a syscall's cost

The runtime's numbers ([`performance.md`](../performance.md)) separate three
things that "the cost of a syscall" hides:

- **how many calls stop in the tracer** — a stop costs about 30 us with
  `-s 500`, and 120-600 us if the tracer sleeps between stops;
- **the cost of one stop** — the wake-up and the two context switches;
- **the tax on every call, stopped or not** — the seccomp filter, about 80 ns
  after the 2026-10-16 change, and 190 ns before it.

An inline hook moves a call from the first axis to the cheapest end: the shim
answers it in the process, about 1 us, where a stop costs 30 us. It does not
change how many syscalls the program makes, and it does not make the kernel
faster; it removes the round trip to the tracer. zpoline's authors measure the
ceiling of that move: 28.1-761 times lower overhead than the exhaustive
alternatives that also do not overwrite instructions they should not touch
([zpoline](#zpoline)).

## The ARM architecture: the sources

Everything below reads the A64 instruction set and the AArch64 platform. The
primary sources, in the order to reach for them:

- **Arm Architecture Reference Manual for A-profile** (DDI 0487) — the
  architecture itself: the register file, the exception model, the memory model,
  cache maintenance. The authority when another page and this one disagree.
- **Arm A64 Instruction Set Architecture** (DDI 0602) — one page per
  instruction. The pages this document relies on: `B`/`BL` (imm26, ±128 MB),
  `ADR`/`ADRP`, `CBZ`/`CBNZ`, `TBZ`/`TBNZ`, `LDR` (literal), `SVC`,
  `DSB`/`DMB`/`ISB`, `DC`/`IC` (cache maintenance), `MRS`/`MSR`. A convenient
  HTML mirror of the same instruction set, generated from Arm's pseudocode, is
  the Stanford reference below.
- **AAPCS64** — the procedure call standard: which registers carry arguments,
  which are callee-saved, and that `X16`/`X17` are `IP0`/`IP1`, reserved for a
  linker's veneers. A hook stub is code generation of the same kind, so it may
  clobber `IP0`/`IP1` and nothing else; this is the rule the arm64 relocators
  obey, and why a prologue using both `X16` and `X17` cannot be hooked.
- **Arm "Learn the Architecture" guides** — the exception model and the memory
  model written for programmers: the exception levels an `SVC` traps to, and the
  ordering barriers (`DMB`/`DSB`/`ISB`) a self-modifying patch depends on.
- **`CTR_EL0`** — the cache-line size the flush loop reads, and the `IDC`/`DIC`
  bits that say whether a step of the flush is redundant on a given core.

An `SVC` is an exception to a higher exception level; a syscall is that trap. A
patched `B` over the `svc` never takes the trap at all, which is the whole point
of the shim: the call is answered in the same exception level it was made in.

## The mechanics of a patch on arm64

An A64 instruction is always four bytes, naturally aligned; the smallest patch
is therefore one instruction. Its meaning can still depend on where it sits:
every PC-relative form encodes an offset **from its own address**, so moving it
byte-for-byte changes its target. The forms, with the field and reach
([A64 ISA](#a64-isa); [KoHook](#kohook)):

| instruction | what it computes | reach |
| --- | --- | --- |
| `ADR` | `Xd = PC + imm` | ±1 MB |
| `ADRP` | `Xd = (PC & ~0xFFF) + (imm << 12)` | ±4 GB, page granular |
| `B` / `BL` | branch, optionally with link | ±128 MB |
| `B.cond` | conditional branch | ±1 MB |
| `CBZ` / `CBNZ` | compare with zero, branch | ±1 MB |
| `TBZ` / `TBNZ` | test one bit, branch | ±32 KB |
| `LDR` (literal) | load a word near the instruction | ±1 MB |

A relocator either recomputes the immediate for the new address (only if the
target still fits the field, which is often narrower) or replaces the
instruction with an absolute sequence.

A patch that is one `B` needs the target within ±128 MB. When the OS places the
hook further away, the fix is a **veneer**: a small stub inside the branch's
reach that jumps the full 64-bit distance
([ARM, "Overview of veneers"](#arm-veneers)). arm64 has no single long-jump
instruction, so a far branch materializes the address and branches indirectly:
`LDR X16, #8; BR X16; .quad target` (16 bytes), or
`ADRP+ADD+LDR+BR` (16 bytes plus the pointer). `X16`/`X17` (`IP0`/`IP1`) are
reserved by the ABI for exactly this, so a veneer may clobber them
([KoHook](#kohook); [frida-gum](#frida-gum)).

A hook engine therefore needs memory for veneers **within ±128 MB of the patch
site**. Two ways to get it: reuse unused padding or symbol space inside the
target ELF, which is co-located by definition (ByteDance's shadowhook, "island
trampolines"), or allocate a fresh region near the target
([shadowhook](#shadowhook); Dobby's `dobby_set_near_trampoline`).

## Trampoline construction

The engines surveyed ([KoHook](#kohook); [Detours](#detours);
[frida-gum](#frida-gum)) do the same five steps:

1. disassemble forward from the entry until at least the patch size is covered
   (4 bytes on arm64 for a `B`, 5-14 on x86-64);
2. for each covered instruction, classify it: copy verbatim, or relocate if it
   depends on its own address;
3. append a jump from the trampoline back to `entry + covered length`;
4. write the redirect over the entry's first bytes;
5. flush the instruction cache before any thread runs the new bytes.

Detours hand-rolls a table-driven decoder only to find instruction lengths and
adjusts IP-relative displacements ([Detours `disasm.cpp`](#detours)). frida-gum
resolves every PC-relative form to a literal load into `X16` and branches
through it; it documents the consequence that a prologue already using both
`X16` and `X17` cannot be hooked ([frida-gum](#frida-gum)). Conditional branches
have no long form, so they are restructured: invert the condition to skip over
an unconditional far jump.

## The instruction cache, W^X, and Android

arm64 does not keep the instruction and data caches coherent for self-modifying
code; the compiler's `__builtin___clear_cache` (a no-op on x86) exists for this.
The sequence for a modified range, per ARM's own blog, is
`DC CVAU` (clean to the point of unification), `DSB ISH`, `IC IVAU`, `DSB ISH`,
`ISB`, repeated per cache line, with the line size from `CTR_EL0`
([ARM, `__clear_cache`](#arm-clearcache)). Omitting it fails **intermittently**:
whether a core sees stale bytes depends on whether it happened to have them
cached, so it reads as "one run in twenty" and differs by device
([KoHook](#kohook) §4).

W^X forbids a page being writable and executable at once, so a patch toggles:
an anonymous trampoline is mapped RW, written, then `mprotect` RX; an existing
library's text (mapped RX from the file) is `mprotect`-ed writable, written, and
restored, never holding both at once. On Android the two operations are separate
SELinux checks ([KoHook](#kohook) §4, citing Android SELinux material):

- **`execmem`** — making an anonymous mapping executable (the trampoline);
- **`execmod`** — modifying a file-backed executable mapping (the library's
  text, which is what the shim does to libc).

AOSP leaves `execmem` broadly enabled for its JIT, but that is an
accommodation, not a guarantee; OEM or hardened builds can tighten it. A related
Android 10 change is about persistence, not in-memory hooking: an app targeting
API 29+ may not modify, through a `dlopen`ed file, code that must be written
back to disk. The ordinary in-memory patch is unaffected
([Android 10 changes](https://developer.android.com/about/versions/10/behavior-changes-10)).
**XOM** (execute-only text) blocked reading a library's bytes to hook it, but was
Android 10 only and was removed in Android 11
([XOM](https://source.android.com/docs/security/test/execute-only-memory)).

## The memory model and the barriers

AArch64 is weakly ordered: two memory accesses may become visible to another
observer in an order different from program order unless a barrier or an ordered
access forces otherwise. Three barriers matter to a patch
([A64 ISA](#a64-isa); [ARM ARM](#arm-arm)):

| barrier | what it guarantees |
| --- | --- |
| `DMB` | orders memory accesses: the accesses before it are observed before those after it |
| `DSB` | like `DMB`, and also waits for the accesses before it to complete |
| `ISB` | flushes the pipeline: following instructions are fetched after the `ISB` |

The flush sequence above is a chain of these: the `DSB` after `DC CVAU` makes the
written bytes visible to the point of unification before `IC IVAU` invalidates
the instruction cache; the second `DSB` waits for that invalidation; the `ISB`
makes the core fetch the new bytes. Drop one and the failure is an ordering, not
the content — a core may keep running bytes it already holds.

Atomicity is a separate guarantee. A naturally aligned load or store, up to a
size the implementation must support, is single-copy atomic: no observer sees a
half-written value. That size always covers an aligned four-byte word, so
replacing one aligned A64 instruction (one `B`) with another is atomic — the
basis of the thread-safety argument below. Ordered loads and stores
(`LDAR`/`STLR`, acquire/release) and the large atomic accesses (`ST64B`) are the
wider forms.

The model itself is formal and machine-readable, so a question about a specific
ordering can be answered rather than argued ([herdtools7](#herdtools7)); Linux's
`memory-barriers.txt` is the practical translation
([Linux barriers](#linux-barriers)).

## Thread-safety of a live patch

A hook write covers a few bytes while another core may be fetching from the same
address. arm64 guarantees single-copy atomicity for a naturally aligned access up
to a size that covers one aligned four-byte word, so **replacing one aligned
instruction with one aligned `B` is race-free at the write**; no lock is needed.
x86 never gets this for a 5-byte or 14-byte overwrite. shadowhook documents a
known, accepted race on 32-bit/Thumb and none on 64-bit ([shadowhook](#shadowhook)).
frida-gum, for some paths, suspends threads, and its release notes record a
fixed suspend deadlock and a careful RW → commit → RX → i-cache-flush ordering
([frida-gum](#frida-gum)).

## Dynamic binary instrumentation: where inline hooking sits

A 2025 survey ([arXiv 2508.00682](#dbi-survey)) splits DBI into three building
blocks (instrumentation, introspection, execution manipulation) and classifies
the instrumentation techniques:

- **JIT-assisted dynamic binary translation** (Pin, DynamoRIO, Valgrind): the
  original code is translated into a code cache and executed there; every
  instruction visible, at the cost of translating it.
- **Dynamic probe injection (DPI)**: instructions are replaced with a jump into
  analysis code (Frida Interceptor, Dyninst) — the family the shim belongs to.
- **Traps and interrupts**: software and hardware breakpoints, single-stepping,
  page faults.
- **CPU interpretation**: an emulator's fetch-decode-execute loop.

Its evaluation finds no technique best in all cases; cost depends on the program
and on how often the instrumented event fires. Valgrind and DynamoRIO add more
detail ([Valgrind design](#valgrind), [DynamoRIO](#dynamorio)).

## The kernel's own nets: seccomp and SUD

Before any in-process patch, the kernel can route a syscall to a handler. The
seccomp filter ([Linux seccomp docs](#seccomp)) returns one of:

- `SECCOMP_RET_TRACE` — notify a `ptrace` tracer, which `dn-tiny` uses
  (`PTRACE_O_TRACESECCOMP`, `PTRACE_EVENT_SECCOMP`);
- `SECCOMP_RET_TRAP` — deliver `SIGSYS`, which Android's own app filter does;
- `SECCOMP_RET_USER_NOTIF` — send a message on a listener fd the supervisor
  reads with `ioctl(SECCOMP_IOCTL_NOTIF_RECV)` and answers with
  `SECCOMP_IOCTL_NOTIF_SEND`; `SECCOMP_IOCTL_NOTIF_ADDFD` injects a file
  descriptor. This is a supervisor path that avoids the `ptrace` stop, and is
  worth measuring against the tracer's 30 us stop.

**Syscall User Dispatch** (SUD, Linux 5.11) delivers `SIGSYS` for calls outside
an allowed address range, switched by a user-memory selector byte with no kernel
call. Neither phone here has it (kernel 4.14 and 5.10;
[`prior-art.md`](prior-art.md), "Syscall User Dispatch"). The kernel's
*trampoline* work is the mirror image: the arm64 bpf trampoline patchset adds
long-jump support when a trampoline and its target exceed the branch range
([LWN](#bpf-tramp)).

## Rewriting the syscall instruction itself

- **zpoline** (x86-64): replace the two-byte `syscall`/`sysenter` with a
  two-byte `call *%rax` and place a trampoline at virtual address 0, reached
  because `rax` holds the (small) syscall number. Reported 28.1-761 times lower
  overhead than the alternatives, exhaustive, and it does not overwrite
  instructions it should not ([zpoline](#zpoline)).
- **lazypoline** (x86-64): SUD as the slow path that finds every `syscall`
  instruction, then a rewrite of each into a direct call — the fast path — so
  the kernel is left out on later runs ([`prior-art.md`](prior-art.md),
  "Syscall User Dispatch").
- **svc-hook** (arm64): at load, every `svc` becomes a branch to a trampoline —
  wider than patching only the recognizable `mov x8,#nr; svc` pair, reaching
  libc's generic `syscall()` ([`prior-art.md`](prior-art.md)).
- **ASC-Hook**: rewrites the pair (the load of `x8` and the `svc`), and names the
  open question the shim shares: is `x8` correct when a jump lands inside the
  pair ([`prior-art.md`](prior-art.md)).

## Engines to read

- **ByteDance shadowhook** — production scale; island trampolines in ELF
  padding; documented 32-bit race and 64-bit atomicity ([shadowhook](#shadowhook)).
- **Dobby** — near-trampoline allocator, `dobby_set_near_trampoline`,
  `ADRP+ADD+LDR+BR` sequence ([Dobby](#dobby)).
- **frida-gum** — arm64 relocator, `X16`/`X17` constraint, thread patch paths
  ([frida-gum](#frida-gum)).
- **Microsoft Detours** — table-driven decoder, IP-relative adjustment
  ([Detours](#detours)).
- **MinHook + HDE** — a minimal length disassembler, for a small engine.
- **Capstone / Zydis** — full decoders; x64dbg moved to Zydis, and Trail of Bits
  found decoder accuracy an unsolved problem, so a decoder bug is a crash risk.

## What each means here

- The shim is a **DPI / inline hook**: one aligned `B` over the `svc` of a
  recognized `mov x8,#nr; svc` pair (the `mov` stays, so the hook reads `x8`;
  a site picked wrongly costs a plain `svc`). By the sources above, that single aligned write is
  atomic on 64-bit Android (no lock), and the shim already flushes the cache
  (`lane.c`, `__builtin___clear_cache`). The technique is sound; nothing here
  says to change it. One deviation from the W^X note above: the shim makes
  libc's text `PROT_READ|PROT_WRITE|PROT_EXEC` if the kernel allows it and falls
  back to `PROT_READ|PROT_WRITE`, rather than toggling in two steps.
- A **near-memory allocator** for veneers (shadowhook's islands, Dobby's
  allocator) is not needed while the stub sits next to libc, but is the first
  thing to build when patching `ld.so`'s 1082 stops ([`performance.md`](../performance.md)),
  whose sites are spread across the loader.
- The **`X16`/`X17`** rule is a correctness check for any stub that needs a
  scratch register.
- **`SECCOMP_RET_USER_NOTIF`** is a candidate to replace a `ptrace` stop on the
  handling side, to be measured.
- The **`execmem`/`execmod`** split and the API-29 rules decide whether the
  whole in-process technique survives on a future Android
  ([`TODO.md`](../../TODO.md), "A prefix in any app").

## Reliability

KoHook is an AI-compiled survey (its own header says "not validated against
silicon"); it is used here for its citations to ARM, Dobby, frida-gum and
shadowhook, which are primary. DBI numbers are each project's or paper's own
report. The Android behaviour (execmem/execmod on a given device, SUD absence) is
measured only where [`device-census.md`](device-census.md) says so.

## Sources

The ARM architecture, primary:

- <a id="arm-arm"></a>Arm, *Arm Architecture Reference Manual for A-profile
  architecture* (DDI 0487) — <https://developer.arm.com/documentation/ddi0487/latest/>
- <a id="a64-isa"></a>Arm, *A64 Instruction Set Architecture* (DDI 0602) —
  <https://developer.arm.com/documentation/ddi0602/latest/>
- <a id="stanford-a64"></a>Stanford, *A64 instruction reference* (HTML, generated
  from Arm's pseudocode) — <https://www.scs.stanford.edu/~zyedidia/arm64/>
- <a id="aapcs64"></a>Arm, *Procedure Call Standard for the Arm 64-bit
  Architecture (AAPCS64)* —
  <https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst>
- <a id="learn-arm"></a>Arm, *Learn the Architecture* guides —
  <https://developer.arm.com/documentation/102374/latest/>
- <a id="herdtools7"></a>The machine-readable AArch64 memory model, herd7
  (herdtools7) — <https://github.com/herd/herdtools7>
- <a id="linux-barriers"></a>Linux, *Memory barriers* (`memory-barriers.txt`) —
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/memory-barriers.txt>

The technique:

- <a id="kohook"></a>KoHook, *Track 1: Native Inline / Trampoline Hooking (ARM64
  & x86)* — <https://github.com/tonytranrp/KoHook/blob/main/research/01-native-inline-hooking/README.md>
- <a id="zpoline"></a>Yasukata et al., *zpoline: a system call hook mechanism
  based on binary rewriting*, USENIX ATC 2023 —
  <https://www.usenix.org/conference/atc23/presentation/yasukata>
- <a id="dbi-survey"></a>Llorente-Vazquez et al., *Unveiling Dynamic Binary
  Instrumentation Techniques*, arXiv:2508.00682, 2025 —
  <https://arxiv.org/html/2508.00682v1>
- <a id="seccomp"></a>Linux, *Seccomp BPF* (userspace API) —
  <https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html>
- <a id="bpf-tramp"></a>Xu Kuohai, *bpf trampoline for arm64*, LWN 900453 —
  <https://lwn.net/Articles/900453/>
- <a id="arm-veneers"></a>ARM, *Overview of veneers* (armlink) —
  <https://developer.arm.com/documentation/dui0803/a/Image-structure-and-generation/Overview-of-veneers>
- <a id="arm-clearcache"></a>ARM, *Caches, self-modifying code, implementing
  `__clear_cache`* —
  <https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-implementing-clear-cache>
- <a id="shadowhook"></a>ByteDance, *android-inline-hook (shadowhook)* —
  <https://github.com/bytedance/android-inline-hook>
- <a id="dobby"></a>jmpews, *Dobby* — <https://deepwiki.com/jmpews/Dobby>
- <a id="frida-gum"></a>Frida, *gumarm64relocator.c* —
  <https://github.com/frida/frida-gum/blob/main/gum/arch-arm64/gumarm64relocator.c>
- <a id="detours"></a>Microsoft, *Detours* `disasm.cpp` —
  <https://github.com/microsoft/Detours/blob/main/src/disasm.cpp>
- <a id="valgrind"></a>Valgrind, *Design and implementation* —
  <https://valgrind.org/docs/manual/design-impl.html>
- <a id="dynamorio"></a>DynamoRIO — <https://github.com/DynamoRIO/dynamorio>
