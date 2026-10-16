# The public research behind the runtime

The long ideas and the papers the runtime draws on, beside the projects in
[`prior-art.md`](prior-art.md). By the "stable net, fast path" rule
([`../spec/mapped-kernel.md`](../spec/mapped-kernel.md)) an idea is worth
trusting when the *concept* is old and the *platform* keeps it; each entry says
which. Names and venues are the published ones; nothing here is measured on this
phone unless it says so.

## The cost of a system call, and how it is removed

The cost is the trap -- the user/kernel mode switch. Every idea below removes it
in a different way.

- **vDSO** (*virtual dynamic shared object*) -- the kernel's own answer: a
  userland page for `clock_gettime`, `gettimeofday`, `getcpu`, so the call takes
  no trap at all. It is the baseline a fast path can never beat, and the model
  for everything else. Kernel; both phones.
- **Exception-less system calls** -- **FlexSC** (Soares & Stumm, OSDI 2010):
  per-thread shared-memory pages that a kernel thread polls, so the common call
  is a memory write, not a trap. This is the precise name for "beat the syscall
  by not trapping". The phones' kernels have no such out-of-band mechanism, so
  it is a direction, not a drop-in.
- **Batching / amortizing** -- **io_uring** (Linux 5.1): a ring of submissions
  and completions, many calls per trap. The S10e lacks it and the app's filter
  kills it on the S23 FE (`device-census.md`).

## Interposing a system call

- **seccomp-BPF** -- a filter over `struct seccomp_data`; `RET_TRACE` (wake a
  `ptrace` tracer), `RET_TRAP` (`SIGSYS` to the process), `RET_USER_NOTIF` (a
  listener fd -- [`kernel-mechanisms.md`](kernel-mechanisms.md)), and the
  **constant-action bitmap** (5.11) that makes an allowed call free.
- **ptrace** -- the portable net, and what `strace` is built on; the tracer is
  the same shape, deliberate about its cost (a stop, `performance.md`).
- **zpoline / lazypoline** -- the syscall *instruction* rewritten to a direct
  call to the interposer, x86-64 in zpoline and with SUD plus an isolated
  interposer in lazypoline (DSN 2024). This is the shape a deb-native fast path
  would take on arm64, and the paper names the exact constraint the shim shares
  (a jump that lands inside a patched pair).
- **Systrap** (gVisor) -- trap once, patch the site, so the next call skips the
  trap. The "learn the site from a real trap" idea of the in-process branch
  (E12); its block is the handler that does not survive `exec`.
- **gVisor** -- the largest working instance of per-call emulation: a user-space
  kernel (`Sentry`) with a `ptrace`/systrap/KVM platform. The measure of how far
  the approach scales.

## A kernel in a directory: root emulation

- **PRoot against fakechroot** (the [`prior-art.md`](prior-art.md) rows):
  dynamic translation inside a tracer against `LD_PRELOAD` in the libc. The
  runtime is the same family, with one shared law instead of per-call rules.
- **Charliecloud, zero-consistency root emulation** (arXiv 2405.06085): the
  seccomp-only "fake root" that keeps no state against `fakeroot`/PRoot that
  keep it. The trade the runtime's fake root sits on.
- **WSL1** -- the closest ancestor: per-call translation of an unmodified Linux
  on a foreign kernel (NT), with `lxcore.sys` in the place `ptrace` and the shim
  hold here.

## Running a program without `execve`

- The **userland exec** lineage (grugq; `ulexecve`, `userlandexec`): map the
  ELF and its `PT_INTERP` loader, build the stack and `auxv`, and jump in. The
  design of `dn-exec` in TODO, "A prefix in any app".

## The other direction: a library OS or a unikernel

- **Gramine**, **OSv**, **LKL** (Linux Kernel Library): rebuild the program or
  its libc to call a library instead of the kernel -- the opposite of patching an
  unmodified libc. They show what a runtime does *not* have to intercept when it
  owns the program; deb-native's bet is the other side, keeping the program
  stock.

## What to read first

1. **vDSO** and **FlexSC** -- the two ends of "remove the trap".
2. **zpoline/lazypoline** and **Systrap** -- the two shapes of an in-process
   fast path (rewrite the instruction; learn the site).
3. **Charliecloud's paper** -- what a fake root can and cannot be.
