# deb-native

A self-contained Debian `arm64` userland in a tarball — its own glibc, `apt`
and files.

## Build

Build host: an `arm64` Debian userland (or CI) with `gcc`, `make`,
`libtalloc-dev`, `dpkg-deb`, `wget`, `xz` and `python3`.

### 1. glibc bundle

Apply the Android patch ([`patches/dn-glibc-android.patch`](patches/dn-glibc-android.patch))
to a Debian glibc source tree and package this project's build of `libc6` and
`libc-bin`:

```sh
scripts/glibc/dn-apply-glibc-patch.sh SRC_TREE PREFIX
scripts/glibc/dn-package-glibc.sh    REAL_LIBC6_DEB DESTDIR libc6.deb
scripts/glibc/dn-package-libc-bin.sh REAL_LIBC_BIN_DEB DESTDIR libc-bin.deb
```

(CI: [`.github/workflows/build-glibc.yml`](.github/workflows/build-glibc.yml).)

### 2. runtime overlay

`dn-shim.so`, `dn-run`, `dn-trace`, `dn-elf` — plain-gcc glibc programs:

```sh
scripts/build/build-overlay-glibc.sh            # → src/.build-glibc/
```

### 3. prefix artifact

Assemble a prefix from a core-ultra tree and package it with its `.dn/`
contract. Inputs: a patched-glibc prefix whose loader is the patched build, the
overlay dir, the artifact's build path, and the pinned package list.

```sh
DN_GLIBC_PREFIX=BUILT_PREFIX \
DN_OVERLAY=src/.build-glibc \
PREFIX_ROOT=/where/the/prefix/will/live \
DEB_LIST=packages.tsv \
  scripts/build/build-core-deb.sh BASE core-deb.tar.gz

scripts/build/package-prefix.sh core-deb \
  --root /where/the/prefix/will/live --name core-deb
```

`BASE` is a core-ultra tree ([`docs/spec/prefix-layers.md`](docs/spec/prefix-layers.md));
cut one from an unpacked prefix with `scripts/build/cut-core-ultra.py SRC DST`.

### Install (on the host)

```sh
scripts/host/ship-prefix.sh core-deb.tar.gz DEST
```

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE) and [`CREDITS.md`](CREDITS.md).
