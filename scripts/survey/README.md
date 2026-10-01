# scripts/survey/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Compatibility surveys and the 0.1.x prototype pipeline, run manually
against package samples. See [`../../docs/log/survey-0.2.0.md`](../../docs/log/survey-0.2.0.md)
for the latest results.

- `survey.sh` — installs a random sample of real Debian `.deb`s, each
  into a fresh isolated prefix, and records where each one fails.
- `survey-apt.sh` — same idea, but installing through real apt instead
  of bare `dpkg` on a single `.deb`.
- `survey-prefix.sh` — the 0.2.0-prealpha survey: installs and runs a
  package list in the 0.2.0 prefix from the same fresh state each time.
- `sample-packages.py` — draws a random, reproducible package sample for
  `survey.sh`/`survey-apt.sh`, mirroring sudo-less's own methodology.
- `survey-sample.py` — picks a random, in-scope survey sample for
  `survey-prefix.sh` from a Debian Packages index.
- `scope-sample.py` — picks an in-scope package sample (by Debian
  Section) from a Packages index, for building the shim-coverage corpus.
- `prototype-install.sh` — the 0.1.x prototype: installs one `.deb` into
  a prefix with stock dpkg relocation flags, no apt. Inactive; see
  [`../../docs/spec/classic-design.md`](../../docs/spec/classic-design.md).
