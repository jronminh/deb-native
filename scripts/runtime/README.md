# scripts/runtime/

<!-- template: templates/readme.template.md -->

Front end used after a prefix already exists. See
[`../../docs/spec/design.md`](../../docs/spec/design.md), "Day-to-day commands".

- `make-shell-interface.sh` — makes the userland the default session
  (`~/.termux/shell` -> `~/.dn-login`) and the host an explicit, nested door
  (`termux-shell`, `dn-shell`), with the welcome and the `pkg` guard
  (`docs/spec/host-userland.md`).
- `dn-adopt.sh` — makes a glibc arm64 program obtained outside apt (a
  release download, a direct installer) run through the prefix.
- `make-launchers.sh` — exposes a prefix's installed programs by name:
  one launcher entry per program, first on `PATH`.
- `make-apt-wrappers.sh` — installs `termux-apt`/`termux-dpkg`,
  `termux-dn-doctor` and `dn-adopt` as commands in the launcher dir.
