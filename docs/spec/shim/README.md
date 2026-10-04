# docs/spec/shim/

> Template: [`templates/readme.template.md`](../../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

The path-redirect shim: the mechanism, what it covers, and what escapes
it. What libc interposition *cannot* see at all is a platform fact and
lives in [`../../reference/syscall-boundary.md`](../../reference/syscall-boundary.md).

- `path-shim.md` — the shim's design, verification, and the ways a glibc
  target can be made to load it.
- `shim-coverage.md` — which libc entry points the shim covers
  (`coverage/` holds the measured corpus data).
- `runtime-failures.md` — what goes wrong when *running* a program,
  grouped by cause.
