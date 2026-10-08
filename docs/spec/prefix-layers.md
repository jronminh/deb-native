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
- [`runtime.md`](runtime.md) -- the planned runtime (dn-policy, dn-glibc,
  dn-trace). Its principle 5 is what lets all three layers below share one
  build of dn-policy/dn-glibc/dn-trace per (CPU arch, `libc6` ABI version)
  instead of rebuilding per layer.

## The layers

| artifact | built from | done when |
| --- | --- | --- |
| **core-ultra** | the patched glibc (the loader and the libraries the rest needs), the shim, `dn-run`, `bash`/`dash`, basic tools, CA certificates, `.dn/` | the prefix's shell runs |
| **core-deb** | the core-ultra recipe + the Debian layer: `dpkg`, `apt` and their libraries; the translation hooks (`dn-hook-pre`/`-post`, `dn-translate-deb`, `normalize-symlinks`, `dn-fix-glibc`, `dn-fix-alternatives`); `priv/`; the apt configuration | `apt install` works |
| **specialized** | the core-ultra recipe + a payload (e.g. the Claude binary) | its payload runs |

Each row is one artifact, installed once and complete. There is no module
mechanism: an installed core-ultra never becomes core-deb; whoever needs the
Debian userland installs core-deb. `apt` and `dpkg` are not a precondition
for having a prefix: a prefix that only runs one program never needs them.

## core-ultra

The smallest prefix a host can install and enter. Its acceptance test is the
same one every install ends with (`prefix-contract.md`): **the prefix's
shell runs**.

Seed packages: Debian's **Essential** set (`base-files`, `base-passwd`,
`bash`, `coreutils`, `dash`, `debianutils`, `diffutils`, `dpkg`, `findutils`,
`grep`, `gzip`, `hostname`, `init-system-helpers`, `libc-bin`,
`ncurses-base`, `ncurses-bin`, `perl-base`, `sed`, `tar`, plus `libc6`). Every
library their ELFs need (`DT_NEEDED`) is added until the set is closed. Plus
the overlay: `dn-shim.so`,
`dn-run`, `dn-trace`, `dn-elf`, `etc/ld.so.preload`, the loader
configuration, and the small `etc` files a shell and a resolver read
(`passwd`, `group`, `hosts`, `nsswitch.conf`, `profile`, `bash.bashrc`,
`inputrc`), and `etc/resolv.conf` as the link to the host's DNS file
(`prefix-contract.md`, build invariant 7).

`dn-run` (with `dn-elf`) is in it so a foreign glibc binary dropped into the
prefix is **adopted on first run** (`dn-run` has `dn-elf` repoint its
interpreter at the prefix loader) -- what makes core-ultra a blueprint.

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

## Minimal core-deb and its restore

core-deb ships **minimal** (the pinned list is
[`scripts/build/packages.tsv`](../../scripts/build/packages.tsv), 54 packages):
enough for the shell, apt and dpkg to run and for apt to fetch the rest --
`libc6`, `libc-bin`, `apt`, `dpkg`, `bash`, `dash`, their libraries
(`libapt-pkg7.0`, `liblz4-1`, `liblzma5`, `libzstd1`, `libbz2-1.0`,
`libselinux1`, `libsystemd0`, ...), the tools package scripts call
(`coreutils`, `sed`, `grep`, `gzip`, `tar`, `xz-utils`, `findutils`, `mawk`,
`debianutils`, `base-files`, `base-passwd`, `perl-base`, ...),
`ca-certificates`, and `debian-archive-keyring`.

The prefix restores the rest itself with apt, from the mirror, once:
`apt-get update`, then `apt-get install` of the names in `.dn/profile` (21,
names only, no pinned versions); a full bootstrap ends at **76 packages**. The
prefix's own bash runs `bootstrap.sh` for this. The two glibc packages,
`libc6` and `libc-bin`, are held (dpkg hold) so apt never replaces the patched
files; the post-invoke hook restores them if a package pulls them in.

`.dn/profile` is a plain list of package names, one per line.

## Naming

`core-ultra` and `core-deb` name **prefix layers** (what an artifact
contains), not repository directories.

## Verified

Both artifacts were cut from one full prefix built for a fixed build path,
then installed with a POSIX shell + toybox alone into another directory,
following `prefix-contract.md`:

| | core-ultra | core-deb |
| --- | --- | --- |
| tarball | 19 MB | 46 MB |
| extracted | 92 MB, 667 files | ~200 MB, ~5000 files |
| relocated | 124 ELF + 28 text | 181 ELF + 139 text |
| checked | the prefix's shell runs; bash, basic commands, fake root, `/mnt`, `sed`, `gzip`; no `apt`/`dpkg` | `bash`, `dpkg`, `apt`, `perl`, `dpkg -l` |

core-ultra as a blueprint: a foreign glibc binary, its interpreter set to a
path absent on the host, placed in the prefix and started through `dn-run`,
was adopted (`dn-run: adopted ... -> prefix loader`) and printed its version.

## Open items

- **Build order**: core-ultra is cut *from* the full prefix today. The layered
  build is the reverse -- build the core-ultra recipe, then add the Debian
  layer for core-deb.
- **Size**: glibc's `gconv` modules are 20 of core-ultra's 92 MB; most can go
  when no charset conversion is needed. Terminfo (`ncurses-base`) is missing
  from today's trimmed prefix.
