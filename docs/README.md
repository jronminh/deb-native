# docs/

Split by what kind of writing a file is, not by topic. Each subdirectory
has its own `README.md` listing its files; this is just the map. When
adding a new doc, put it in the directory that matches what it *is*, not
where it feels thematically closest, and add it to that directory's
`README.md`.

- [`spec/`](spec/README.md) — technical specification: what the project
  does and how, as it stands right now. Read these as ground truth;
  update them when the design changes.
- [`log/`](log/README.md) — chronological or one-off investigation, not
  current state.
- [`guides/`](guides/README.md) — how-to for a specific, one-off case,
  not a spec of the project itself.

A spec doc that gets superseded moves to `log/` (or gets a status note)
rather than being silently edited into agreement with history it never
described. The reverse also happens: when a log turns out to contain
durable reference knowledge (a taxonomy, a catalog, a verdict table) mixed
in with its narrative, pull that part into `spec/` and leave a pointer —
`spec/android-platform.md`/`log/android-seccomp-audit.md` is the example.
