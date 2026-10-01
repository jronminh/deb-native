# scripts/tools/

Standalone diagnostics, not part of any install or runtime path.

- `dn-doctor.sh` — checks (and with `--fix`, repairs) the common
  Termux-prefix seams that go wrong in practice: a leaked `APT_CONFIG`,
  a clobbered Termux `sources.list`, missing launchers, a stale PATH.
