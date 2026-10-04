# Deploy: Debian's `libc6`/`libc-bin`, then the 10-file swap

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing -- a new spec topic, a new one-off
> investigation, or a new guide -- not just a long addition to what a doc
> already covers.

How a `dn-glibc` prefix is deployed: install Debian's **real** `libc6` and
`libc-bin`, then immediately **swap in the 10 files** the Android patch
actually changes. Nothing else about the bootstrap changes. The swapped
files are prefix-agnostic: the loader derives the live prefix from its own
path once at run time (`__dn_prefix_get`, `sysdeps/generic/dn-prefix.h`),
so one prebuilt set fits any prefix. Shipped and verified (0.6.0+s.1); the
prebuilt set ships as the rolling `glibc-bundle` release asset.

## Contents

- [The model](#the-model)
- [The swap set](#the-swap-set)
- [Run-time prefix self-derivation](#run-time-prefix-self-derivation)
- [Install order in the bootstrap](#install-order-in-the-bootstrap)
- [What stays unchanged](#what-stays-unchanged)
- [Open items](#open-items)

## Related docs

- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) -- the prefix this deploys,
  its fixed paths, and the loader patch.
- [`../log/findings/glibc-patch-swap-set.md`](../log/findings/glibc-patch-swap-set.md)
  -- how the 10-file set was derived (source + DWARF).
- [`../log/findings/own-glibc-missing-libc-bin.md`](../log/findings/own-glibc-missing-libc-bin.md)
  -- why `libc-bin` is path-sensitive.
- [`install-flow.md`](install-flow.md) -- the one-time bootstrap this
  plugs into.

## The model

Instead of shipping a full rebuilt glibc, deploy reuses Debian's own
packages for everything our patch does not touch:

1. Install Debian `libc6` and `libc-bin` unmodified (real, held packages).
2. Overwrite the 10 files our Android patch affects with our builds.
3. Continue the ordinary bootstrap; the rest is unchanged.

This keeps Debian as the source of truth for the bulk of glibc and shrinks
the project's own artifact to the 10 files. It also means a Debian point
release only needs a re-check of the 10 files, not a full glibc rebuild.

## The swap set

Ten files, listed in
[`../log/findings/glibc-patch-swap-set.md`](../log/findings/glibc-patch-swap-set.md):

- `libc6` (7): `libc.so.6`, `ld-linux-aarch64.so.1`, `libresolv.so.2`,
  `libnsl.so.1`, `libnss_compat.so.2`, `libnss_hesiod.so.2`, `librt.so.1`.
- `libc-bin` (3): `usr/sbin/ldconfig`, `usr/bin/localedef`,
  `usr/bin/iconv`.

Only `libc.so.6` and the loader (and `ldconfig`, for the cache it writes)
carry run-time prefix derivation; the other seven are swapped for their
other patch effects, and their file access is covered by libc's redirected
opens at run time.

## Run-time prefix self-derivation

For the swapped files to fit **any** prefix, they must not bake the prefix
at compile time. One small mechanism does it: a global **live prefix** cut
once from the loader's own absolute path (`_dl_rtld_map.l_name`) and
exported as the weak-linked `__dn_prefix_get()` (`sysdeps/generic/
dn-prefix.h`). Every internal file path is then built from it via
`__dn_build()` instead of the compiled `@TERMUX_PREFIX@`:

- loader: system search dirs, `preload_file` (`/etc/ld.so.preload`) and the
  cache path (`/usr/etc/ld.so.cache`), computed once before the search
  paths are built (`elf/rtld.c`, `elf/dl-cache.c`);
- libc's own internal opens use the same global (`elf/dl-load.c`, ...);
- `ldconfig` derives its cache/conf/libdirs/aux paths from the live prefix
  (`elf/ldconfig.c`).

Prefix matching requires a path-component boundary, so a prefix whose name
is a prefix of another (`.dn` vs `.dn12`) does not match wrongly. The
compiled prefix stays the fallback when derivation yields nothing. One
consequence: a **static** link leaves `__dn_prefix_get` NULL, so
`__dn_build` yields guest-relative paths -- the static `ldconfig` cannot
self-derive, and the bootstrap bypasses it (see [Open items](#open-items)).

Shipped (0.6.0+s.1): `build-glibc.yml` builds the set on an aarch64 runner
and publishes it as the rolling `glibc-bundle` release; no clean-build
regression remains (verified on CI and by a fresh on-device bootstrap).

## Install order in the bootstrap

`scripts/bootstrap/setup-apt-prefix.sh`, in order:

1. Build the runtime (`setup-runtime.sh`): shim, `dn-run`, `dn-shell`,
   `dn-trace`, priv wrappers.
2. Fetch and verify the Debian index; `dn-debian-index.sh` rewrites
   `Architecture: all` -> `arm64`.
3. **`dn-install-glibc.sh`**: install Debian's real `libc6` then `libc-bin`
   (both held), swap in the 10 files, wire `<prefix>/etc/ld.so.preload`
   (the shim), `<prefix>/usr/etc/ld.so.conf`, and build `ld.so.cache`
   (ldconfig under the tracer -- see Open items).
4. `dn-standins.sh`: the prefix's `dpkg`/`apt` stand-ins.
5. Download the base and its transitive closure, translate each `.deb`,
   unpack and configure in one dpkg call; hold the base set.
6. `setup-runtime` again (idempotent), launchers, apt wrappers, activation.

## What stays unchanged

The bootstrap's base packages, `dn-translate-deb.sh` (PT_INTERP and
maintainer-script rewrites), the apt configuration and hooks, and the
launcher/routing layer are all unchanged. Only the `libc6`/`libc-bin`
acquisition step changes.

## Open items

- **`ldconfig` writer**: `elf/ldconfig.c` derives its cache/conf/libdirs/
  aux paths from the live prefix, but the shipped `ldconfig` is **static**,
  so `__dn_prefix_get` is NULL and `__dn_build` yields empty paths
  (`Renaming of ~ to  failed`). The bootstrap therefore **bypasses** it:
  `dn-install-glibc.sh` runs the static `ldconfig` under the tracer with
  explicit `-C`/`-f` (bare guest paths bound into the prefix) and ignores
  its exit status. A missing `ld.so.cache` is not fatal (programs still
  run). Open: give the static writer a working derivation.
- **Artifact distribution**: the 10 files ship as the rolling
  `glibc-bundle` release asset, fetched by `setup-apt-prefix.sh`. Whether
  to also publish to `deb-native-repo` is still open.
