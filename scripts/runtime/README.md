# scripts/runtime/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Front end used after a prefix already exists. See
[`../../docs/spec/design.md`](../../docs/spec/design.md), "Day-to-day commands".

- `dn-activate.sh` — activates a prefix for the user's shell: a managed
  block in `~/.bashrc` (launcher dir on `PATH`, `apt`/`dpkg` aliases).
- `dn-adopt.sh` — makes a glibc arm64 program obtained outside apt (a
  release download, a direct installer) run through the prefix.
- `make-launchers.sh` — exposes a prefix's installed programs by name:
  one launcher entry per program, first on `PATH`.
- `make-apt-wrappers.sh` — installs `termux-apt`/`termux-dpkg`,
  `termux-dn-doctor` and `dn-adopt` as commands in the launcher dir.
