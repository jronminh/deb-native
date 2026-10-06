# deb-native

A **self-contained Debian userland in a tarball** — real `arm64` `.deb`
packages, their own glibc, apt and files inside one prefix. A **poor host**
installs it with nothing but a POSIX shell and `tar`/`dd`/`sed` (toybox is
enough): no root, no `chroot`, no namespaces, and the host's own tree left
untouched.

> Pre-alpha, AI-assisted, not independently audited. Use a throwaway host.

## How it ships

The prefix is built elsewhere and shipped as a tarball carrying a `.dn/`
contract. There is **no build on the host**.

- **Build** (a build host, `scripts/build/` + `scripts/glibc/`): the runtime
  overlay (`dn-shim.so`, `dn-run`, `dn-trace`, `dn-elf`), the glibc bundle,
  and the prefix artifact with its `.dn/` contract.
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `.dn/contract`, extract, relocate.
- **Activate / complete**: the artifact's `.dn/install.sh` (host shell), then
  `.dn/bootstrap.sh` (the prefix's own shell, installing `.dn/profile`). When
  bootstrap succeeds the prefix is ready.

The contract is described in [`docs/spec/prefix-contract.md`](docs/spec/prefix-contract.md).

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE) and [`CREDITS.md`](CREDITS.md).
