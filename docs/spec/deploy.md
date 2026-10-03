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
files must be prefix-agnostic for this to work under any prefix, which is
what run-time prefix self-derivation provides. Status: design; the
self-derivation build is currently blocked by a clean-build regression
(see below), so deployment uses the last buildable build for now.

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
at compile time. The mechanism and sites are specified in
[`dn-glibc-prefix.md`](dn-glibc-prefix.md), "Runtime prefix
self-derivation"; in short, the loader derives the prefix from its own
path (`_dl_rtld_map.l_name`), libc from `dladdr`, and every internal file
open is rewritten onto the live prefix.

Status: **blocked.** The self-derivation build breaks the clean glibc build
at `elf/librtld.map` with multiple-definition errors (reproduced on CI --
ubuntu-24.04-arm -- and on-device, so it is the patch, not the
environment). Deployment therefore uses the last buildable build for now.
Two edit groups are involved and need bisecting:

- loader: `elf/rtld.c`, `elf/dl-cache.c`, `sysdeps/generic/dn-prefix.h`;
- libc: `sysdeps/unix/sysv/linux/open*/openat*.c`.

## Install order in the bootstrap

1. Bootstrap the base set as today.
2. Install Debian's real `libc6` then `libc-bin` (their loader,
   `ldconfig`, ... land under the prefix).
3. Swap in the 10 files (`cp`/extract over the installed ones).
4. Wire `ld.so.preload` (the shim) at `<prefix>/etc/`, `ld.so.conf` and
   the `ldconfig`-built `ld.so.cache` at `<prefix>/usr/etc/` (see
   `dn-glibc-prefix.md`, "Fixed paths").
5. Continue: `libc6-dev`/toolchain, gcc `specs`, launchers.

The current `scripts/bootstrap/dn-install-glibc.sh` installs our own full
`libc6`/`libc-bin` packages; the overhaul changes it to step 2 + the swap.

## What stays unchanged

The bootstrap's base packages, `dn-translate-deb.sh` (PT_INTERP and
maintainer-script rewrites), the apt configuration and hooks, and the
launcher/routing layer are all unchanged. Only the `libc6`/`libc-bin`
acquisition step changes.

## Open items

- **Fix the self-derivation build regression** (bisect the two groups
  above), then regenerate the patch and validate on CI.
- **`ldconfig` writer**: its cache path (`LD_SO_CACHE`/`LD_SO_CONF`) is
  still baked; it needs the same run-time derivation as the loader.
- **Artifact distribution**: how the 10 files are shipped (release asset
  vs. `deb-native-repo`) is still open.
