# Foreign libc in the app domain

**Measured 2026-10-07** on a Samsung Galaxy S23 FE, Android 16, kernel
5.10.240, in `untrusted_app_27` (target API 28). The stock binaries below
started from a system shell in the app's process tree with an empty
environment. The [syscall sweep](android-seccomp-syscall-map.md) supplies the
per-number data. The scope is one device and one set of binaries.

| Binary | Observed result |
| --- | --- |
| Static Go test built without cgo | File queries, eight goroutines, child execution, and a Unix socket test passed |
| Static Debian trixie glibc test | `SIGSYS` on `set_robust_list` before `main`. Disabling glibc's `rseq` registration did not change the outcome |
| Alpine static PIE BusyBox 1.37.0 | Startup succeeded. An applet's `setgid` call triggered `SIGSYS` |
| Dynamic test linked against the experimental patched glibc | `accept()` returned a descriptor. The patched library used `accept4` |

## Coverage of the early prefix runtime

An early prefix used a preload for virtual identity and invoked the tracer
only on selected exec paths. With virtual root enabled, all 12 tested
identity-related libc functions succeeded in a dynamic test. In the same
test, three identity syscalls issued by number and a direct syscall
instruction still triggered `SIGSYS`. With virtual root disabled, eight of
the tested functions triggered `SIGSYS`.

A static test invoked nine identity syscalls. Eight triggered `SIGSYS`
from both the system shell and the prefix shell. All nine completed when
the test was run explicitly under the historical tracer. The same tracer
also allowed the stock static glibc test to start. This is evidence about
the early exec routing. The [current architecture](../docs/architecture.md)
describes the tracer's exec gate and the in-process fast paths.

FOA describes the coverage of symbol and tracer answers in the
[identity method](https://github.com/jronminh/fhs-on-android/blob/main/methods/identity.md) and the platform constraints in
the [reference model](https://github.com/jronminh/fhs-on-android/blob/main/reference/problem.md).
