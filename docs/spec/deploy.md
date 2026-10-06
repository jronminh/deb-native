# Deploy: Debian's `libc6`/`libc-bin`, then the 10-file swap

<!-- template: templates/docs.template.md -->

How the project's glibc is assembled: use Debian's **real** `libc6` and
`libc-bin`, then immediately **swap in the 10 files** the Android patch
actually changes. The swapped files are prefix-agnostic: the loader derives
the live prefix from its own path once at run time (`__dn_prefix_get`,
`sysdeps/generic/dn-prefix.h`), so one prebuilt set fits any prefix. It is
built on a build host (`scripts/glibc/`, `.github/workflows/build-glibc.yml`)
and assembled into the prefix artifact by `scripts/build/build-core-deb.sh`;
the target never builds.

## Contents

- [The model](#the-model)
- [The swap set](#the-swap-set)
- [Run-time prefix self-derivation](#run-time-prefix-self-derivation)
- [Where it happens now](#where-it-happens-now)
- [Open items](#open-items)

## Related docs

- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) -- the prefix this deploys,
  its fixed paths, and the loader patch.
- [`prefix-contract.md`](prefix-contract.md) -- the artifact the assembled
  glibc ships in.
- [`prefix-layers.md`](prefix-layers.md) -- core-ultra and core-deb.

## The model

Instead of shipping a full rebuilt glibc, reuse Debian's own packages for
everything our patch does not touch:

1. Take Debian `libc6` and `libc-bin`.
2. Overwrite the 10 files our Android patch affects with our builds.
3. Everything else is Debian's, unchanged.

This keeps Debian as the source of truth for the bulk of glibc and shrinks
the project's own payload to the 10 files. A Debian point release therefore
needs only a re-check of the 10 files, not a full glibc rebuild.

## The swap set

Ten files:

- `libc6` (7): `libc.so.6`, `ld-linux-aarch64.so.1`, `libresolv.so.2`,
  `libnsl.so.1`, `libnss_compat.so.2`, `libnss_hesiod.so.2`, `librt.so.1`.
- `libc-bin` (3): `usr/sbin/ldconfig`, `usr/bin/localedef`, `usr/bin/iconv`.

Only `libc.so.6` and the loader carry run-time prefix derivation; the other
eight are swapped for their other patch effects, and their file access is
covered by libc's redirected opens at run time.

## Run-time prefix self-derivation

For the swapped files to fit **any** prefix, they must not bake the prefix at
compile time. One small mechanism does it: a global **live prefix** cut once
from the loader's own absolute path (`_dl_rtld_map.l_name`) and exported as
the weak-linked `__dn_prefix_get()` (`sysdeps/generic/dn-prefix.h`). Every
internal file path is then built from it via `__dn_build()` instead of the
compiled `@DN_PREFIX@`:

- loader: system search dirs, `preload_file` (`/etc/ld.so.preload`) and the
  cache path (`/usr/etc/ld.so.cache`), computed once before the search paths
  are built (`elf/rtld.c`, `elf/dl-cache.c`);
- libc's own internal opens use the same global (`elf/dl-load.c`, ...);
- `ldconfig` derives its cache/conf/libdirs/aux paths from the live prefix
  (`elf/ldconfig.c`).

Prefix matching requires a path-component boundary, so a prefix whose name is
a prefix of another (`.dn` vs `.dn12`) does not match wrongly. The compiled
prefix stays the fallback when derivation yields nothing. One consequence: a
**static** link leaves `__dn_prefix_get` NULL, so `__dn_build` yields
guest-relative paths -- the static `ldconfig` cannot self-derive.

## Where it happens now

At **build**, `scripts/build/build-core-deb.sh` extracts the pinned Debian
packages (libc6 and libc-bin included), overwrites the 10 files from a
`DN_GLIBC_PREFIX` whose loader is the patched build, translates interpreter
paths with `dn-elf`, and lays the runtime overlay. The **target** ships the
artifact (`scripts/host/ship-prefix.sh`) and, when it carries a `.dn/profile`,
completes it (`scripts/host/bootstrap-prefix.sh`).

There is no `ld.so.cache` (prefix-contract build invariant): with it absent,
the loader derives its library dirs from the live prefix, so no `ldconfig`
run and no tracer bind is needed during an install.

## Open items

- **`ldconfig` writer**: the shipped `ldconfig` is static, so
  `__dn_prefix_get` is NULL and `__dn_build` yields empty paths. Nothing in
  the current flow needs it (no `ld.so.cache`); if a cache is ever shipped,
  the writer needs a working derivation.
- **Artifact distribution**: the glibc bundle ships as a build input; the
  assembled prefix artifact ships as a tarball. Whether to publish
  pre-translated packages to `deb-native-repo` is still open.
