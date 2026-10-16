# What is stable: the syscall ABI and the instruction set

The shim and the policy rest on things that must not move under them. Some are
very stable (the kernel's syscall ABI, the base A64 instructions); some are not
(glibc's internal code). Telling them apart is what decides which mechanism to
lean on.

## The Linux syscall ABI — very stable

- **The numbers.** arm64 uses one generic table (since 3.7): a number means the
  same syscall on every arm64 kernel, and the kernels only *add*
  ([`syscalls-by-kernel.md`](syscalls-by-kernel.md)). A number is a permanent
  contract — no kernel reuses one for a different call.
- **The interface.** `x8` holds the number, `x0..x5` the arguments, the result
  comes back in `x0`. The semantics are documented "Linux 1.0+" and are not
  changed; syscalls are removed essentially never (a removed number would be an
  `ENOSYS` hole for the programs that used it).

So a **policy keyed by number**, and a **seccomp filter keyed by number**, rest
on a very stable contract.

## The base A64 instruction set — very stable

- The base A64 instructions are permanent: `svc`, the general-purpose registers,
  the program counter. Later architecture versions *add* (SVE, SME, LSE, ...);
  they do not change the base. arm64 is also fixed-width (all instructions 4
  bytes), which is why a rewrite is clean.
- seccomp exposes `seccomp_data.instruction_pointer` — the program counter at the
  syscall — a stable, architected interface.

So an **instruction-pointer rule** (the gate of
[`experiments.md`](experiments.md), E13) rests on the ISA, not on any program's
code.

## What is NOT stable

- **glibc's internal code**: which function holds the `svc`, the exact
  `mov x8,#nr; svc` sequence, the cancellable path's `x6` convention, whether a
  call goes through a given function. These are a compiler's and a source's
  output and change between glibc versions. Identifying the **cancellable path**
  sits here (E9-E10, rejected).
- A shared library's **symbol set** and behaviour: mostly stable, but a library
  contract, not the kernel's.

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

## Checked: the ABI net catches what the shim cannot reach

A program calling `syscall(SYS_openat, AT_FDCWD, "/etc/passwd", ...)` — a raw
syscall, bypassing the shim's `openat` symbol — opens the file correctly under
`dn-tiny`: the tracer's filter catches `nr 56` and maps the path (measured
`fd=3`, read `root:x:0:0:ro`). The cost is one stop. So the stable ABI net
(seccomp by number) covers the calls the shim cannot reach — the raw `syscall()`
path, and anything without a symbol — with no instruction-level trick. This is
the design's floor: correctness from the ABI, speed from answering in-process
where a symbol exists.
