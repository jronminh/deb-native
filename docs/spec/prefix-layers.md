# Prefix layers: core-ultra and core-deb

<!-- template: templates/docs.template.md -->

The recipe builds two prefixes. **`core-ultra`** is the minimal prefix --
the patched glibc, a shell and basic tools -- and is done when its shell runs.
**`core-deb`** is the same recipe plus the Debian layer -- `apt`, `dpkg` and
the package translation pipeline -- and is done when `apt install` works. The
layering is a **build-time** recipe only: each is shipped and installed as its
own self-contained artifact, and nothing is ever added into an installed
prefix as a module. A **specialized prefix** is built the same way, from the
core-ultra recipe plus its payload. Status: design; both artifacts exist as
experiments cut from one full prefix (sizes below).

## Contents

- [The layers](#the-layers)
- [core-ultra](#core-ultra)
- [core-deb](#core-deb)
- [Specialized prefixes](#specialized-prefixes)
- [Naming](#naming)
- [Verified](#verified)
- [Open items](#open-items)

## Related docs

- [`prefix-contract.md`](prefix-contract.md) -- the `.dn/` interface every
  layer's artifact carries, and how a host installs one.
- [`../../MODULARIZE.md`](../../MODULARIZE.md) -- Build vs Ship; the build
  stage that produces these artifacts.
- [`install-flow.md`](install-flow.md) -- today's bootstrap, which builds the
  full prefix in one pass.

## The layers

| artifact | built from | done when |
| --- | --- | --- |
| **core-ultra** | the patched glibc (the loader and the libraries the rest needs), the shim, `dn-run`, `dn-shell`, `bash`/`dash`, basic tools, `patchelf`, CA certificates, `.dn/` | the prefix's shell runs |
| **core-deb** | the core-ultra recipe + the Debian layer: `dpkg`, `apt` and their libraries; the translation hooks (`dn-hook-pre`/`-post`, `dn-translate-deb`, `patch-scripts-tree`, `normalize-symlinks`, `dn-fix-alternatives`, `dn-fix-glibc`); `priv/`; `make-launchers`; the maintainer-script interpreters; `debconf`, the archive keyring, the apt configuration | `apt install` works |
| **specialized** | the core-ultra recipe + a payload (e.g. the Claude binary) | its payload runs |

Each row is one artifact, installed once and complete. There is no module
mechanism: an installed core-ultra never becomes core-deb; whoever needs the
Debian userland installs core-deb. `apt` and `dpkg` are not a precondition
for having a prefix: a prefix that only runs one program never needs them.

## core-ultra

The smallest prefix a host can install and enter. Its acceptance test is the
same one every install ends with (`prefix-contract.md`): **the prefix's
shell runs**.

Seed packages (their files, from dpkg's own lists): `libc6`, `bash`, `dash`,
`coreutils`, `sed`, `grep`, `tar`, `gzip`, `findutils`, `mawk`,
`debianutils`, `base-files`, `base-passwd`, `libtinfo6`, `ca-certificates`,
`patchelf` with `libstdc++6` and `libgcc-s1`. Every library their ELFs need
(`DT_NEEDED`) is added until the set is closed. Plus the overlay: `dn-shell`,
`dn-shim.so`, `dn-run`, `etc/ld.so.preload`, the loader configuration, and
the small `etc` files a shell and a resolver read (`passwd`, `group`,
`hosts`, `nsswitch.conf`, `profile`, `bash.bashrc`, `inputrc`), and
`etc/resolv.conf` as the link to the host's DNS file (`prefix-contract.md`,
build invariant 7).

`patchelf` and `dn-run` are in it so a foreign glibc binary dropped into the
prefix is **adopted on first run** (`dn-run` repoints its interpreter at the
prefix loader) -- what makes core-ultra a blueprint.

**`.dn/packages`** lists the Debian packages a prefix contains, one per
line (`package<TAB>version<TAB>arch`). core-ultra has no `dpkg` database, so
this is the only record of what it is made of (for listing, and for knowing
when a package it carries has a security update).

## core-deb

The Debian userland, as today's full prefix is: built from the core-ultra
recipe plus the Debian layer, shipped and installed as one artifact.

## Specialized prefixes

A specialized prefix is **its own artifact, built from the core-ultra
recipe plus a payload**, and installed like the two cores: once, complete.
Nothing is added to an installed core-ultra to make one.

Example: **`claude`** -- core-ultra plus the official Claude Code binary
(a glibc program obtained outside apt). The build places the binary in the
prefix and treats it like every other glibc ELF: its `PT_INTERP` gets the
256-byte capacity and the prefix's loader, and its offset goes into
`.dn/baked-paths`, so it is relocated with the rest at install and runs
directly, with no adoption at run time. `.dn/contract` names it
(`name=claude`, an `entry` that starts the shell or the program). It is built
where builds happen (in a core-deb prefix, or a build host) and lands in the
host's store like any artifact. Such a prefix needs no `apt`, `dpkg` or
Debian base beyond core-ultra.

Run-time adoption (`dn-run` repointing a foreign binary's interpreter on its
first run) stays in core-ultra for binaries a user brings in later; it is not
how a specialized prefix is made.

## Naming

`core-ultra` and `core-deb` name **prefix layers**. The repository's `core/`
directory names a **code module** (translation logic, shim, tracer;
`MODULARIZE.md`); the two are unrelated.

## Verified

Both artifacts were cut from one full 0.7.1-dev prefix built for
`/data/data/org.dn.shell/files/core`, then installed with Android's `mksh` +
toybox alone (`env -i PATH=/system/bin /system/bin/sh`) into another
directory, following `prefix-contract.md`:

| | core-ultra | core-deb |
| --- | --- | --- |
| tarball | 19 MB | 46 MB |
| extracted | 92 MB, 667 files | ~200 MB, ~5000 files |
| relocated | 124 ELF + 28 text | 181 ELF + 139 text |
| checked | `dn-shell -c 'exit 0'`; bash, 140 commands, fake root, `/mnt`, `sed`, `gzip`; no `apt`/`dpkg` | `bash`, `dpkg`, `apt`, `perl`, `dpkg -l` (61 packages) |

core-ultra as a blueprint: the official Claude Code binary with its
interpreter set to `/lib/ld-linux-aarch64.so.1` (absent on Android), placed
in the prefix and started through `dn-run`, was adopted
(`dn-run: adopted ... -> prefix loader`) and printed its version.

## Open items

- **Build order**: core-ultra is cut *from* the full prefix today. The layered
  build is the reverse -- build the core-ultra recipe, then add the Debian
  layer for core-deb -- and touches the bootstrap order in
  `bootstrap/setup-apt-prefix.sh`.
- **`dn-run` and `dn-trace` are Bionic**: built with Termux's clang because
  the bootstrap runs `ldconfig` through the tracer while installing the glibc
  swap. With no `ld.so.cache` in an artifact (`prefix-contract.md`, "Build
  invariants") that dependency goes, so both can be built with the prefix's
  own gcc against its glibc -- which a host without a Bionic toolchain needs
  in order to rebuild them. Open: the shim is then preloaded into them too
  (the `access()` rewrite trap of lazy adopt, and the tracer's own paths).
  core-ultra carries `dn-run` but not `dn-trace` (its `libtalloc` is a
  Bionic library from Termux today).
- **Size**: glibc's `gconv` modules are 20 of core-ultra's 92 MB; most can
  go when no charset conversion is needed. Terminfo (`ncurses-base`) is
  missing from today's trimmed prefix.
