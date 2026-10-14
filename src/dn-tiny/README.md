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
time from its own tables: a syscall is caught only when the tracer maps a path in it,
gates it (exec), answers it at entry (fake root) or rewrites its result, and only a
result to rewrite asks for the exit stop. The rows are ordered hottest first, since the
filter tests them in order.

Two copies keep a stop cheap. The path walk reads each component's link through a
cache (`policy.c`) whose generation lives in `var/lib/deb-native/dn-tiny.gen`, a file
of the prefix every `dn-tiny` on it maps: a call that can make, remove or move a link
(`symlinkat`, `unlinkat`, `linkat`, `renameat(2)`) moves the generation at entry and
again at the process's next stop, when the call is over, so every session drops its
entries; without the file there is no cache. The working directory of each process
(`trace.c`) is kept the same way, dropped by `chdir`, `fchdir`, a rename or an unlink,
and when the process is gone or its pid comes back in a fork.

Build and run:

```sh
sh scripts/build/build-tiny.sh OUTDIR          # dn-tiny, dn-substtest, dn-census
OUTDIR/dn-tiny -t PREFIX_ROOT -- /usr/bin/bash /boot/init.sh COMMAND ...
```

`-t` is the prefix root (a host path). `-m bare|regs|full` and `-c CPU` are for
measurements, `-q` silences the summary line.
