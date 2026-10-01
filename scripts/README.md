# scripts/

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
- [`bench/`](bench/README.md) — benchmarking and syscall/symbol scanning,
  not part of any install path.
- [`survey/`](survey/README.md) — compatibility surveys and the 0.1.x
  prototype pipeline, run manually against package samples.

Scripts reference each other across these directories with
`$HERE/../<category>/<script>.sh` (`$HERE` = the calling script's own
directory), not by name alone — keep that pattern when moving or renaming
a script.
