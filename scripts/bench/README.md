# scripts/bench/

Benchmarking and syscall/symbol scanning — not part of any install or
runtime path, run manually.

- `bench-tracer.sh` — benchmarks fork-lite (bind-only tracing) against a
  reference `proot` on path-op-dense workloads.
- `perf-run.sh` — runs a command under a Termux wake-lock so Android
  doesn't demote or kill it while Termux isn't the foreground app.
- `scan-libc-symbols.sh` — scans ELFs for the dynamic libc symbols they
  import, to find which path-taking entry points the shim must intercept.
  See [`../../docs/spec/shim-coverage.md`](../../docs/spec/shim-coverage.md).
- `scan-direct-syscalls.py` — scans ELFs for direct syscall usage (raw
  `svc #0`, no libc symbol) that the shim can't see at all. See
  [`../../docs/spec/syscall-boundary.md`](../../docs/spec/syscall-boundary.md).
