# deb-native

A self-contained Debian `arm64` userland in a tarball.

## Build (the glibc parts)

An `arm64` Linux host with `gcc`, `make`, `libtalloc-dev`, `python3`.

- **glibc bundle** — `scripts/glibc/` repackages Debian's own `libc6`/`libc-bin`
  with this project's Android-patched build. Heavy, so it runs in CI
  ([`.github/workflows/build-glibc.yml`](.github/workflows/build-glibc.yml)),
  published as the rolling `glibc-bundle` release.
- **runtime overlay** — `scripts/build/build-overlay-glibc.sh` builds
  `dn-shim.so`, `dn-run`, `dn-trace`, `dn-elf` into `src/.build-glibc/`.

## Package (the tarball)

Assemble a prefix and write its `.dn/` contract — a host with `dpkg-deb`,
`wget`, `xz`:

```sh
DN_GLIBC_PREFIX=BUILT_PREFIX DN_OVERLAY=src/.build-glibc \
PREFIX_ROOT=ROOT DEB_LIST=packages.tsv \
  scripts/build/build-core-deb.sh BASE core-deb.tar.gz
scripts/build/package-prefix.sh core-deb --root ROOT --name core-deb
```

`BASE` is a core-ultra tree (`scripts/build/cut-core-ultra.py`); details in
[`docs/spec/prefix-layers.md`](docs/spec/prefix-layers.md).

## Install

```sh
scripts/host/ship-prefix.sh core-deb.tar.gz DEST
```

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE), [`CREDITS.md`](CREDITS.md).
