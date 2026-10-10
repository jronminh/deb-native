# deb-native

A self-contained Debian `arm64` userland in a tarball.

## Build

An `arm64` Linux host with `gcc`, `make`, `libtalloc-dev`, `python3`.

```sh
scripts/build/build-overlay-glibc.sh    # → src/.build-glibc/
```

## Package

A host with `dpkg-deb`, `wget`, `xz`.

```sh
DN_GLIBC_PREFIX=... DN_OVERLAY=src/.build-glibc \
PREFIX_ROOT=... DEB_LIST=... \
  scripts/build/build-core-deb.sh BASE core-deb.tar.gz
scripts/build/package-prefix.sh core-deb --root ... --name core-deb
```

## Install

On a host with `curl` (or toybox `wget`) and `tar`, fetch the rolling `prefix`
release and ship it to a destination you choose (the artifact is relocatable:
dn-trace derives the prefix root from its own location):

```sh
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/scripts/host/install-from-release.sh | sh -s -- core-deb "$HOME/.dn"
```

The second argument is `DEST` (default `./core-deb`). `core-ultra`, or from a
checkout:

```sh
scripts/host/install-from-release.sh core-ultra /data/local/deb-native
scripts/host/install-from-release.sh core-deb
```

Or ship a tarball already on disk:

```sh
scripts/host/ship-prefix.sh core-deb.tar.gz DEST
```

## Boot

The host boots the tree by running the contract's `entry` from the prefix
root (`install.sh` writes the same line to `DN_SESSION_SHELL` if the host sets
it). `dn-trace` derives the tree from its own location, so the artifact installs
at any path:

```sh
cd DEST
usr/lib/deb-native/dn-trace
```

With no program, `dn-trace` execs the prefix's init, which sets the environment
and, on a first boot, completes the prefix itself (`dpkg --configure -a`, then
`apt update` + the `.dn/profile` packages), then opens an interactive shell.
Pass a program instead of the default shell:

```sh
usr/lib/deb-native/dn-trace -- /usr/bin/bash /usr/lib/deb-native/init.sh \
    /usr/bin/dpkg -l
```

See [`docs/spec/prefix.md`](docs/spec/prefix.md), "Boot".

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE).
