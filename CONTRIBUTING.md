# Contributing

Pre-alpha, mostly single-author, AI-assisted — but reports, repros and patches
are welcome.

## Reporting a bug

Use the [bug report](https://github.com/jronminh/deb-native/issues/new/choose)
form. Include the phone and Android version, the prefix's `boot/version`, the
command and what it printed, and, if you can, the same command run without the
shim (move `usr/lib/deb-native/dn-lane.so` aside) and with `DN_LANE_PATCH=0`.
Security problems: see [`SECURITY.md`](SECURITY.md).

## Working on the code

Read [`AGENTS.md`](AGENTS.md) first; it holds the conventions. The essentials:

- **It runs on a phone.** There is no CI and no container that can run it: build
  and test in Termux on an `arm64` phone ([`docs/building.md`](docs/building.md)).
- **Test what you change** in a prefix: build the runtime, copy it into a prefix
  (`docs/building.md`, "Updating the runtime in a prefix"), and run the programs
  your change touches, with and without the shim.
- **Docs state the current truth**; history lives in git.

## Pull requests

Keep them small and focused, say what you tested and on which phone, and match
the surrounding style. No CLA; contributions are under the project license
(GPL-3.0-or-later).
