# dn-tiny

A freestanding tracer: no libc, no imports, one static aarch64 executable of about
70 KB. It runs a program, and every program it starts, under a seccomp filter with
`SECCOMP_RET_TRACE` for the syscalls the policy lists, and answers them. It is a
prototype of what `dn-monitor` does for paths, fake root and the exec gate, without
PRoot's code. What it does, what it was measured to cost and what a real bootstrap
needed: [`docs/reference/ptrace-interception.md`](../../docs/reference/ptrace-interception.md).
What is still open: `TODO.md`, "Freestanding tracer".

Four parts, one concern each:

| file | part |
| --- | --- |
| `monitor.c` | the catcher: builds the filter from the policy's rows, runs the wait loop, reads and writes the tracee's registers and memory |
| `trace.c` | the applier: carries out what a row says (map a path, fake an answer, rewrite a result, answer a denied call, copy for a link) |
| `exec.c` | the exec gate: scripts and glibc programs started through the prefix's loader |
| `policy.c`, `policy.h` | the table and the law: which syscalls, where the path arguments are, how a path is resolved against the prefix, what a denied call is told |

`substtest.c` is a guest program that calls the substituted and faked syscalls and
prints what it is told; `census.c` tries every syscall number and reports which ones
Android's filter kills (`docs/reference/device-census.md`). Both are built beside
`dn-tiny` and use no libc either.

`sys.c`, `sys.h` hold the raw aarch64 syscalls, `_start` and the few string
functions the rest uses. `gen_rows.py` writes the rows (`policy-rows.inc`) at build
time from the syscall tables in `src/tracer`; those tables come from PRoot, which is
why the credits still name it.

Build and run:

```sh
sh scripts/build/build-tiny.sh OUTDIR          # dn-tiny, dn-substtest, dn-census
OUTDIR/dn-tiny -t PREFIX_ROOT -- /usr/bin/bash /boot/init.sh COMMAND ...
```

`-t` is the prefix root (a host path). `-m bare|regs|full` and `-c CPU` are for
measurements, `-q` silences the summary line.
