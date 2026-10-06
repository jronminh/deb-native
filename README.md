# deb-native

A self-contained Debian `arm64` userland in a tarball.

## Build

Needs an `arm64` glibc host with `gcc`, `make`, `libtalloc-dev`, `dpkg-deb`,
`wget`, `xz`, `python3`.

```sh
scripts/build/build-overlay-glibc.sh                       # → src/.build-glibc/
scripts/build/build-core-deb.sh BASE out.tar.gz            # + the env below
scripts/build/package-prefix.sh out --root ROOT --name core-deb
```

`build-core-deb.sh` takes `DN_GLIBC_PREFIX` (a patched-glibc prefix),
`DN_OVERLAY` (= `src/.build-glibc`), `PREFIX_ROOT` (where the prefix will live),
`DEB_LIST` (pinned packages). `BASE` is a core-ultra tree
(`scripts/build/cut-core-ultra.py`); the glibc bundle is `scripts/glibc/`
(CI: `.github/workflows/build-glibc.yml`). Details:
[`docs/spec/prefix-layers.md`](docs/spec/prefix-layers.md).

## Install

```sh
scripts/host/ship-prefix.sh out.tar.gz DEST
```

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE), [`CREDITS.md`](CREDITS.md).
