# docs/spec/tracer/

> Template: [`templates/readme.template.md`](../../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

The syscall tracer (`dn-trace`), used for what the shim cannot reach:
static and raw-syscall binaries, and NSS. The tracer's source lives in
[`../../../tracer/`](../../../tracer/README.md).

- `tracer.md` — the tracer itself (fork-lite, `dn-trace`, the SIGSYS
  emulation, what it costs).
- `bind-only.md` — the bind-only fast path and its scope assumptions.
- `direct-usage.md` — living investigation into what bypasses the shim and
  what the tracer does about it.
