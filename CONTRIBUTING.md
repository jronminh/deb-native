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

- **It runs on a device.** deb-native targets Termux on unrooted Android, so
  there is no dev container and no CI that runs the program — you need a Termux
  device (with the [requirements](README.md#requirements)) to build and test.
  Where the maintainer's reference device is a phone reached over SSH, yours is
  whatever you have.
- **Test where it runs.** Edit anywhere, then build and run the on-device tests
  there — `tests/` each have their own `run.sh` — before you commit.
- **Run one command at a time** — no chaining, no parallel jobs.
- **Docs state only the current state**; history lives in `docs/log/`. New docs
  and directory READMEs follow [`templates/`](templates/README.md).
- After moving or renaming a doc or script, run
  `scripts/tools/check-repo.py`.

CI on pull requests runs only **static** checks (`check-repo.py`, shell and
Python syntax); it cannot run the program. Say what you tested, and on what.

## Pull requests

Keep them small and focused, say what you tested and on which device, and match
the surrounding style. No CLA; contributions are under the project license
(GPL-3.0-or-later).
