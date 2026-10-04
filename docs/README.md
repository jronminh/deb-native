# docs/

<!-- template: templates/readme.template.md -->

Split by what kind of writing a file is, not by topic. Each subdirectory
has its own `README.md` listing its files; this is just the map. When
adding a new doc, put it in the directory that matches what it *is*, not
where it feels thematically closest, and add it to that directory's
`README.md`.

- [`spec/`](spec/README.md) — the project's own design: what it does and
  how, as it stands right now. Read these as ground truth; update them
  when the design changes. Subdirs: `shim/`, `tracer/`.
- [`reference/`](reference/README.md) — look-up material, not a design of
  ours: facts about the platform, ABI and formats the project leans on,
  the support contract, and the known-issues catalog.
- [`notes/`](notes/README.md) — comparisons, positioning, and superseded
  designs kept as the record.
- [`guides/`](guides/README.md) — how-to for a specific, one-off case,
  not a spec of the project itself.
- [`log/`](log/README.md) — chronological or one-off investigation, not
  current state.

The line between the first three is *kind of writing*: `spec/` changes
whenever the system changes; `reference/` is what you consult (platform
behaviour, measured data, catalogs); `notes/` is not current state at all.

A spec doc that gets superseded moves to `log/` (or `notes/`, when the
comparison itself is the value) rather than being silently edited into
agreement with history it never described. **From 0.6.0+s.1 a spec states
only the current state: no "was X", no "superseded" banner, no change
narrative — the only history in this repo is [`log/`](log/README.md).**
When a mechanism is retired, its spec moves to `log/` (or is deleted if
`log/` already covers it) and the live specs are rewritten to the new
truth. The reverse also happens: when a log turns out to contain durable
reference knowledge (a taxonomy, a catalog, a verdict table) mixed in with
its narrative, pull that part into `reference/` and leave a pointer —
`reference/android-platform.md`/`log/android-seccomp-audit.md` is the
example.
