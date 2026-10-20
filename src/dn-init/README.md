# dn-init and dn-lane.so

The runtime: `dn-init`, a tracer with no libc (one static aarch64 executable of
about 72 KB), and `dn-lane.so`, the shim it preloads into every glibc program.
How they work together: [`docs/architecture.md`](../../docs/architecture.md).

| file | part |
| --- | --- |
| `catch.c` | the catcher: builds the seccomp filter from the policy's rows, runs the wait loop, reads and writes the tracee's registers and memory |
| `apply.c` | the applier: carries out what a row says (map a path, fake an answer, answer at entry, copy for a link, answer a call the platform killed) |
| `gate.c` | the exec gate: glibc programs and scripts started through the prefix's loader, with the shim preloaded |
| `law.c`, `law.h` | the table and the law: which syscalls, where their paths are, how a path is resolved in the prefix, what a killed call is told. Linked into both engines -- the tracer as it is, the shim `-DDN_LIBC` |
| `gen_rows.py` | writes the rows at build time: a syscall is caught only when the tracer acts on it, hottest first |
| `mon.h`, `apply.h`, `gate.h` | what the parts offer each other |
| `sys.c`, `sys.h` | raw syscalls, `_start`, the few string functions |
| `lane.c`, `lane.h` | the shim, and the lane cookie the filter and the shim share |
| `ldhook.c` | the loader hook (an `LD_AUDIT` module): patches the loader's own path calls to the lane, mapping through the law (`-DDN_FREE`) so the loader's startup costs no stop |
| `substtest.c` | a guest program that calls the faked and substituted syscalls and prints what it is told |
| `census.c` | tries every syscall number and reports which ones the platform kills ([`docs/reference/device-census.md`](../../docs/reference/device-census.md)) |

## Build

`scripts/build-runtime.sh` builds `dn-init`, `dn-substtest` and `dn-census` with
any aarch64 C compiler. `dn-lane.so` is a glibc library, built inside a prefix
with the prefix's own `gcc`, and links the law (`law.c`, with the generated
rows) as well as the shim:

```sh
gcc -O2 -Wall -Wextra -shared -fPIC -DDN_LIBC -Igen \
    -o /usr/lib/deb-native/dn-lane.so lane.c law.c
```

(`gen/policy-rows.inc` is written by `build-runtime.sh`; `scripts/build-prefix.sh`
does it all; [`docs/building.md`](../../docs/building.md)).

## Run

```sh
dn-init [-q] [-s USEC] [-m bare|regs|full] [-c CPU] -t ROOT -- PROGRAM [ARG...]
```

| option | |
| --- | --- |
| `-t ROOT` | the prefix root, a host path |
| `-q` | no summary line at exit |
| `-s USEC` | poll for the next stop for up to USEC microseconds before sleeping |
| `-m bare\|regs\|full` | for measurements: stop and do nothing, read registers only, or the full work (default) |
| `-c CPU` | pin the tracer and its children to one CPU (measurements) |

The summary line: entry and exit stops, events (fork, exec), signal stops,
paths mapped, calls answered at entry, execs, `SIGSYS` answers, the wall time,
then which syscalls the platform killed and how often.

The shim takes `DN_LANE_PATCH=0` from the environment of a program in the prefix:
it then leaves libc's text alone.
