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

Fetch the rolling `prefix` release and ship it (installs to the contract's
`root=`):

```sh
scripts/host/install-from-release.sh            # core-deb
scripts/host/install-from-release.sh core-ultra
```

Or ship a tarball already on disk:

```sh
scripts/host/ship-prefix.sh core-deb.tar.gz DEST
```

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE).
