# deb-native

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**Real Debian `arm64` `.deb` packages inside Termux: no root, no `chroot`,
no namespaces.** `apt install PKG` works and the program runs by name; a
small Debian tree in `~/.dn`, Termux left untouched.

> [!WARNING]
> **Pre-alpha, AI-assisted, not independently audited.** Read `install.sh`
> and `scripts/` before running them. The on-disk layout can change between
> releases, heavy packages (toolchains) aren't fully supported, and this has
> had no security review. Use a throwaway Termux install or device.

## Requirements

Termux with `git`, `clang`, `patchelf` and the glibc side-install
(`glibc-runner`, `coreutils-glibc`, `bash-glibc`, `perl`, the loader and
libraries). Optional: `make` + `libtalloc` (`pkg install make libtalloc`) for
the tracer — without them, static and raw-syscall programs run untranslated.

## Install

```sh
# pinned pre-alpha release:
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/v0.6.0+s.1-prealpha/install.sh | DEB_NATIVE_REF=v0.6.0+s.1-prealpha sh

# then restart Termux — the new session is the Debian userland
apt install figlet
figlet hi
```

Rolling edge: replace both `v0.6.0+s.1-prealpha` with `main`. Idempotent from
a checkout: `sh install.sh [PREFIX] [pkg ...]`; log in `~/.dn/var/log/`.

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

## Documentation

[`docs/README.md`](docs/README.md) maps everything: start with
[`docs/spec/status.md`](docs/spec/status.md) (where it is today) and
[`docs/spec/design.md`](docs/spec/design.md) (how it works). Roadmap in
[`TODO.md`](TODO.md); conventions in [`AGENTS.md`](AGENTS.md); releases in
[`Releases`](https://github.com/jronminh/deb-native/releases).

## Credit & license

Built on other people's work — see [`CREDITS.md`](CREDITS.md).
GPL-3.0-or-later — [`LICENSE`](LICENSE).
