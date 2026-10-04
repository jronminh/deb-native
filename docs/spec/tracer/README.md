# docs/spec/tracer/

<!-- template: templates/readme.template.md -->

The syscall tracer (`dn-trace`), used for what the shim cannot reach:
static and raw-syscall binaries, and NSS. The tracer's source lives in
[`../../../tracer/`](../../../tracer/README.md).

- `tracer.md` — the tracer itself (fork-lite, `dn-trace`, the SIGSYS
  emulation, what it costs).
- `bind-only.md` — the bind-only fast path and its scope assumptions.
- `direct-usage.md` — living investigation into what bypasses the shim and
  what the tracer does about it.
