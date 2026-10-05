# Findings: apt hooks live in the prefix, not the checkout (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** The runtime translate/index hooks the prefix's apt
runs were pointed at an absolute path into the deb-native **checkout**
(`<checkout>/core/install/...`). Moving or removing the checkout silently
stopped translating runtime installs (the second half of
[`silent-untranslated-runtime-installs.md`](silent-untranslated-runtime-installs.md)).

## Contents

- [Problem](#problem)
- [Fix](#fix)
- [Existing prefixes](#existing-prefixes)

## Problem

`setup-apt-prefix.sh` wrote the hooks into `$DN/etc/apt.conf` as

```
DPkg::Pre-Install-Pkgs { "$INSTALL/dn-hook-pre.sh $DN"; };
DPkg::Post-Invoke       { "$INSTALL/dn-hook-post.sh $DN"; };
APT::Update::Post-Invoke-Success { "$INSTALL/dn-debian-index.sh ..."; };
```

with `$INSTALL=$HERE/../install` expanded to the checkout. A prefix is meant to
be self-contained; depending on an external checkout for its own package
translation is not, and a moved checkout made the hook path dangle -- apt just
skips a missing hook, so installs went back to untranslated silently.

## Fix

- New `core/runtime/install-hooks.sh` copies the hook set and everything they
  call **inside** the prefix under `$DN/usr/lib/deb-native/`, keeping the repo
  layout so the hooks' relative paths still resolve:
  - `core/install/*.sh` (the hooks + helpers),
  - `core/runtime/make-launchers.sh` (`dn-hook-post.sh` calls it),
  - `core/bench/scan-direct-syscalls.py` (`make-launchers.sh` calls it),
  - `custom/*.sh` (per-package fixes; `dn-translate-deb.sh` calls
    `$HERE/../../custom/<pkg>.sh`).
- `setup-apt-prefix.sh` calls it and points `apt.conf` at
  `$DN/usr/lib/deb-native/core/install/...` instead of the checkout.
- `install.sh`'s refresh branch calls it too, so an update refreshes the copies.

Moving the checkout no longer affects a prefix's apt.

## Existing prefixes

A prefix bootstrapped before this still has the checkout path baked into
`apt.conf`. One-time fix: re-point the hook paths and copy the hooks in
(`install-hooks.sh` then replace the checkout prefix with
`$DN/usr/lib/deb-native/scripts` in the `apt.conf` hook lines). Applied to the
live prefix here; verified an `apt --reinstall` still translates.
