# deb-native

Real Debian arm64 `.deb` packages inside Termux — no root, no `chroot`, no
namespaces. A small Debian tree beside Termux's own, Termux left untouched.

> Pre-alpha, AI-assisted, not independently audited. Use a throwaway Termux
> install or device.

## How it ships

The prefix is built on a build host and shipped as a tarball. There is no
build on the target.

- **Build** (`build/`): the glibc bundle, the runtime overlay
  (`dn-shim.so`, `dn-run`, `dn-trace`, `dn-elf`), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`).
- **Ship** (`core/runtime/ship-prefix.sh`): read `.dn/contract` without
  extracting, check it, extract, relocate.
- **Activate / complete**: the artifact's own `.dn/install.sh` (host shell)
  then `.dn/bootstrap.sh` (the prefix's own shell, installing `.dn/profile`
  from the mirror). When bootstrap succeeds the prefix is ready.

The contract is described in `docs/spec/prefix-contract.md`.

## License

GPL-3.0-or-later — see `LICENSE` and `CREDITS.md`.
