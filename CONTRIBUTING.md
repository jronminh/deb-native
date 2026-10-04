# Contributing

Pre-alpha, mostly single-author, AI-assisted — but reports, repros and patches
are welcome.

## Reporting a bug

Use the [bug report](https://github.com/jronminh/deb-native/issues/new/choose)
form, and check
[`docs/reference/known-issues.md`](docs/reference/known-issues.md) first. Include
the `termux-dn-doctor` output and the package state (`dpkg -l <package>`) — most
issues here come down to those. Setup questions go to
[Discussions](https://github.com/jronminh/deb-native/discussions). Security
problems: see [`SECURITY.md`](SECURITY.md).

## Working on the code

Read [`AGENTS.md`](AGENTS.md) first; it is the conventions doc. The essentials:

- **The phone is the source of truth.** The reference setup is Termux on a
  device, reached over SSH; edit locally, sync there, build and test there,
  then commit and push from there.
- **Run one command at a time** — no chaining, no parallel jobs.
- **Docs state only the current state**; history lives in `docs/log/`. New docs
  and directory READMEs follow [`templates/`](templates/README.md).
- After moving or renaming a doc or script, run
  `scripts/tools/check-repo.py`.

## Pull requests

Keep them small and focused, say what you tested and on which device, and match
the surrounding style. No CLA; contributions are under the project license
(GPL-3.0-or-later).
