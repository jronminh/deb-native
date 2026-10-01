# scripts/tools/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Standalone diagnostics, not part of any install or runtime path.

- `dn-doctor.sh` — checks (and with `--fix`, repairs) the common
  Termux-prefix seams that go wrong in practice: a leaked `APT_CONFIG`,
  a clobbered Termux `sources.list`, missing launchers, a stale PATH.
- `check-repo.py` — checks the repo itself, not a prefix: broken markdown
  links, broken table-of-contents anchors, and scripts nothing calls any
  more. Run after moving/renaming/deleting a doc or a script.
