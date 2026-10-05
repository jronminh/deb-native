# scripts/runtime/

<!-- template: templates/readme.template.md -->

Front end used after a prefix already exists. See
[`../../docs/spec/design.md`](../../docs/spec/design.md), "Day-to-day commands".

- `dn-adopt.sh` — makes a glibc arm64 program obtained outside apt (a
  release download, a direct installer) run through the prefix; `--scan`
  adopts every candidate ELF under a directory.
- `dn-update.sh` — installs one prebuilt deb-native overlay component
  (runtime, loader, shims, hooks, launchers) over its fixed location, within
  an allowlist; the updater for the parts `apt` cannot touch.
- `make-launchers.sh` — exposes a prefix's installed programs by name:
  one launcher entry per program, first on `PATH`.
- `install-hooks.sh` — copies the apt translate/index hook scripts (and the
  files they call) inside the prefix, so the prefix's own `apt` translates
  packages without depending on the checkout's path.

The target-specific pieces live in `adapters/deb-native/`:
`make-shell-interface.sh` (userland login/switch, welcome, `pkg` guard) and
`make-apt-wrappers.sh` (`termux-apt`/`termux-dpkg`/`termux-dn-doctor`/`dn-adopt`).
