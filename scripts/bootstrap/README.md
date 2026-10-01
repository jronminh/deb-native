# scripts/bootstrap/

Build the prefix itself, once. See [`../../docs/spec/design.md`](../../docs/spec/design.md).

- `setup-apt-prefix.sh` — the main entry point: bootstraps a self-contained
  prefix (its own apt/dpkg database, `libc6` stand-in, Debian base).
- `bootstrap-base.sh` — 0.1.x's one-transaction base install; inactive,
  superseded by `setup-apt-prefix.sh`'s own base stage.
- `dn-standins.sh` — builds and installs the prefix's stand-in packages
  (`libc6`, `dpkg`, `apt` under Debian's names, Termux's content).
- `dn-package-glibc.sh` — packages this project's own-built glibc
  (0.5.0) as a real `libc6` `.deb`, instead of a Termux-glibc stand-in.
- `build-path-redirect.sh` — cross-compiles `native/path-redirect.c`
  (the shim) into a glibc shared library with Termux's own clang.
