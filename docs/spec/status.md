# Project status

<!-- template: templates/docs.template.md -->

What works today, the scope it holds to, and what is not built yet. A
living snapshot, not a changelog: the roadmap is [`TODO.md`](../../TODO.md),
and confirmed breakages are [`../reference/known-issues.md`](../reference/known-issues.md).

## Contents

- [What works](#what-works)
- [Scope and proof](#scope-and-proof)
- [Not yet](#not-yet)
- [Health](#health)

## Related docs

- `design.md` — how the whole thing works, end to end.
- `../reference/standard.md` — the package-scope contract.
- `../reference/known-issues.md` — the confirmed breakages.

## What works

- A Debian package installs to dpkg status `ii` and its program runs by name.
- `apt`/`dpkg` are the prefix's, and the default session is the Debian
  userland. `termux-shell` opens a clean Termux shell (for `pkg`,
  `termux-apt`); a `pkg` guard refuses inside the userland.
- Toolchains: `apt install gcc`, a full compile (`libc6-dev`), and running the
  result.

## Scope and proof

The same packages as [`sudo-less`](https://github.com/jronminh/sudo-less), by
Debian section ([`../reference/standard.md`](../reference/standard.md)). 99 of
100 random Debian 13 packages installed and ran, with no tracer needed
(`../log/survey-0.2.0.md`).

## Not yet

- **Services:** a package that ships a unit installs, but the service does not
  run; `runit` is the plan.
- **Real root:** packages needing system users, `setuid`, TUN or kernel modules
  do not work; `sudo` modes are planned.
- **Heavy toolchains** are not fully supported (some compile; `rustc`/`ghc` are
  unresearched).

## Health

`termux-dn-doctor` checks the common breakages. Confirmed issues:
[`../reference/known-issues.md`](../reference/known-issues.md).
