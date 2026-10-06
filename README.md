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
PREFIX_ROOT=... DEB_LIST=packages.tsv \
  scripts/build/build-core-deb.sh BASE core-deb.tar.gz
scripts/build/package-prefix.sh core-deb --root ... --name core-deb
```

## Install

```sh
scripts/host/ship-prefix.sh core-deb.tar.gz DEST
```

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE).
