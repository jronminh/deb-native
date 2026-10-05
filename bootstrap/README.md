# bootstrap/

<!-- template: templates/readme.template.md -->

Build the prefix itself, once. See [`../docs/spec/design.md`](../docs/spec/design.md).

- `setup-apt-prefix.sh` — the main entry point: bootstraps a self-contained
  prefix (its own apt/dpkg database, this project's own `libc6`/`libc-bin`,
  Debian base).
- `dn-install-glibc.sh` — installs this project's own-built, prefix-targeted
  `libc6`/`libc-bin` (real, held packages) and wires the fused loader's
  `ld.so.preload`/`ld.so.conf`/`ld.so.cache` -- the `dn-glibc` prefix every
  fresh install gets, no `ld-dn`.
- `dn-standins.sh` — builds and installs the prefix's stand-in packages
  (`dpkg`, `apt` under Debian's names, Termux's content).
- `dn-package-glibc.sh` — packages this project's own-built glibc
  (0.5.0) as a real `libc6` `.deb`, instead of a Termux-glibc stand-in.
- `dn-package-libc-bin.sh` — packages this build's own glibc *programs*
  (`ldconfig`, `ldd`, `getconf`, `locale`, ...) as a real `libc-bin` `.deb`
  — needed because `ldconfig` is path-sensitive (writes the prefix's
  `ld.so.cache`), unlike `libc6-dev`/`libc-dev-bin`.
- `build-dn-shim.sh` — cross-compiles `core/native/dn-shim.c`
  (the shim) into a glibc shared library with Termux's own clang.
