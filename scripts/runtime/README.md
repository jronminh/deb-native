# scripts/runtime/

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
