# scripts/

> Template: [`templates/readme.template.md`](../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Grouped by where each script sits in a prefix's lifecycle, plus three
standalone categories. Each subdirectory has its own `README.md` listing
its scripts and what each one does; this file is just the map. When
adding a new script, drop it into the matching directory below (and add
it to that directory's `README.md`) rather than back at `scripts/` top
level.

- [`bootstrap/`](bootstrap/README.md) — build the prefix itself, once.
- [`install/`](install/README.md) — runs on every package install, via
  apt hooks or directly.
- [`runtime/`](runtime/README.md) — front end used after a prefix exists.
- [`tools/`](tools/README.md) — standalone diagnostics.
- [`integrate/`](integrate/README.md) — the half-fusion exposure layer
  (skeleton; see [`../docs/spec/half-fusion.md`](../docs/spec/half-fusion.md)).
- [`bench/`](bench/README.md) — benchmarking and syscall/symbol scanning,
  not part of any install path.
- [`survey/`](survey/README.md) — compatibility surveys and the 0.1.x
  prototype pipeline, run manually against package samples.

Scripts reference each other across these directories with
`$HERE/../<category>/<script>.sh` (`$HERE` = the calling script's own
directory), not by name alone — keep that pattern when moving or renaming
a script.
