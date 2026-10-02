# scripts/integrate/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

**Skeleton — idea only, no code yet.** This is where the half-fusion
exposure layer will live: the piece that selectively and reversibly
surfaces parts of the Debian prefix (`~/.dn`) into Termux without
overwriting anything Termux owns. Design and rationale:
[`../../docs/spec/half-fusion.md`](../../docs/spec/half-fusion.md).

Planned files (nothing implemented on the branch yet):

- `expose-commands.sh` — build the `$PREFIX/opt/dn/bin` symlink farm from
  `$DN/usr/bin` (+ `sbin`), applying the collision policy.
- `expose-tree.sh` — `$PREFIX/opt/dn -> ~/.dn`, a read-only view.
- `sync-config.sh` — opt-in shared config/data (`resolv.conf`, CA bundle,
  timezone, XDG/`$HOME` wiring).
- `dn-unfuse.sh` — remove exactly what was added, from the manifest;
  `$PREFIX` returns to byte-identical.
- `collisions.toml` — `[policy] default = abort | termux | debian |
  qualify` and `[prefer] name = "side"`.
- `home.toml` — `[home] shared`, `namespace`, `rc`.

Rules this directory must keep (see the spec's "Invariants"):
additive only, never overwrite a Termux path, never expose `$DN/usr/lib`
to a Bionic process, and always reversible.
