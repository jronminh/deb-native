# The prefix: the tree and the tarball

<!-- template: templates/docs.template.md -->

A **prefix** is a self-contained Debian `arm64` userland in one tarball: its
own glibc, `apt`, and files, with the overlay from [`overlay.md`](overlay.md)
at `RT` inside it. This doc covers how a prefix is **built** (a build host with
a toolchain and network) and **shipped** (a poor host: a POSIX shell and
toybox). It is the artifact contract — what the build produces, what `.dn/`
carries, and the steps a host runs. Status: living doc; the build and ship
stages below are what the scripts do.

## Contents

- [The model](#the-model)
- [Layers](#layers)
- [The `.dn/` directory](#the-dn-directory)
- [The contract file](#the-contract-file)
- [Build](#build)
- [Ship](#ship)
- [Completion](#completion)
- [Host updates](#host-updates)
- [Open items](#open-items)

## Related docs

- [`overlay.md`](overlay.md) — the runtime the tree carries (`dn-policy`,
  `dn-glibc`, `dn-trace`), and the glibc rule.
- [`../reference/`](../reference) — background on Android's syscall and
  filesystem behaviour.

## The model

An artifact is one tarball. It carries `home/`, `root -> home`, and
`mnt -> ../mnt`, all relative, so a host that has a sibling `mnt/` sees it and
one that does not gets a dangling link. Extract it where it was built and it
runs as is; installing it is `tar x` plus two short scripts.

The build **fixes** `TREE` (the prefix root) and `RT` at build time. Every
glibc ELF keeps the interpreter Debian gave it
(`/lib/ld-linux-aarch64.so.1`); the kernel never resolves that path, because
every exec goes through [`overlay.md`](overlay.md)'s exec gate, which runs a
glibc-dynamic program through `RT/ld.so` itself. A package is therefore
installed intact, and **nothing repoints a `PT_INTERP` after the build**.

The consequence: the artifact is built for one absolute path, `PREFIX_ROOT`,
and is **not position-independent**. The loader self-derives the tree from its
own path, but a few tree config files (`etc/ld.so.conf`, `etc/apt/apt.conf.d/`)
name `PREFIX_ROOT` absolutely. The host installs the artifact at that path (or
the build targets the path the host chose); relocating later is not supported
(see [Open items](#open-items)).

## Layers

One build produces up to three artifacts, each shipped and installed on its
own; nothing is ever added into an installed prefix as a module:

| artifact | built from | done when |
| --- | --- | --- |
| **core-deb** | an empty base + the pinned package list + the overlay | `apt install` works |
| **core-ultra** | a cut of the core-deb tree: the minimal seed packages + the overlay | the prefix's shell runs |
| **specialized** | the core-ultra recipe + a payload (e.g. the Claude binary) | its payload runs |

**core-deb** ships minimal (the pinned list is
[`../../scripts/build/packages.tsv`](../../scripts/build/packages.tsv), 54
packages): enough for the shell, apt and dpkg to run and for apt to fetch the
rest. The prefix restores the remainder itself from the mirror, once, from the
names in `.dn/profile` (21 names); a full bootstrap ends at **76 packages**.

**core-ultra** is cut *from* the core-deb tree by
[`../../scripts/build/cut-core-ultra.py`](../../scripts/build/cut-core-ultra.py):
the files of the seed packages, the overlay, the small `etc` files a shell and
a resolver read, and every library their ELFs need, added until the set is
closed. core-ultra has no `dpkg` database, so its `.dn/packages` is the only
record of what it contains.

## The `.dn/` directory

Every artifact carries `.dn/` at its root — the prefix's interface to its host,
outside the FHS tree:

```
<prefix>/
├── .dn/
│   ├── contract        data: key=value, read by the host without running code
│   ├── packages        the Debian packages the prefix contains
│   ├── profile         the packages a bootstrap restores from the mirror
│   ├── baked-paths     files the build path is written into (by the build)
│   ├── install.sh      /system/bin/sh, run by the host's shell: activate
│   └── bootstrap.sh    optional: run by the prefix's own shell, completes it
├── home/
├── root -> home
├── mnt -> ../mnt
├── etc/resolv.conf -> ../../app/etc/resolv.conf
└── usr/ etc/ var/ ...  the tree, as built
```

`.dn/packages` is one `package<TAB>version<TAB>arch` per line.
`.dn/baked-paths` lists every file the build path is written into, one per
line, fields tab-separated, paths relative to the root:

```
text	etc/apt/apt.conf.d/50deb-native
text	usr/etc/ld.so.conf
text	usr/etc/ld.so.conf.d/dn.conf
```

With relocation dropped, an `elf` entry is no longer produced (no `PT_INTERP`
is rewritten); the `text` entries remain, and they are why the artifact is
built for a fixed `PREFIX_ROOT`.

## The contract file

`.dn/contract` is plain text a POSIX shell parses with `while read`: one
`key=value` per line, split at the first `=`; blank lines and `#` ignored; an
unknown key warns but does not fail.

| key | required | meaning |
| --- | --- | --- |
| `contract` | yes | the contract version (integer); this doc is version `1` |
| `name` | yes | the default directory name and the name the host shows |
| `desc` | no | one line describing the prefix |
| `version` | no | the prefix's own version (`VERSION` at the repo root) |
| `arch` | yes | the CPU architecture (`uname -m`) |
| `root` | yes | the absolute directory the prefix's files name (`PREFIX_ROOT`) |
| `loader` | yes | the loader, relative to the root; the one ELF a host shell can exec directly |
| `install` | yes | the activation script, relative to the root |
| `bootstrap` | no | the completion script; absent means the prefix is complete as shipped |
| `entry` | yes | the command, relative to the root, that opens a session |
| `size` | no | the extracted size in MiB, for a free-space check |

Example (core-deb):

```
# dn prefix contract
contract=1
name=core-deb
desc=core-deb: core-ultra plus apt and dpkg
version=0.7.1-dev
arch=aarch64
root=/data/local/deb-native
loader=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
install=.dn/install.sh
bootstrap=.dn/bootstrap.sh
entry=usr/bin/bash -i
size=200
```

## Build

A build host: an `arm64` Linux machine with `gcc`, `make`, `libtalloc-dev`,
`python3`, `dpkg-deb`, `wget`, `xz`. Nothing here runs inside a prefix or on
the target.

1. **The overlay.** `scripts/build/build-overlay-glibc.sh` builds `dn-trace`
   from `src/tracer/` and copies `src/syscalls.tsv` — the whole overlay — into
   `src/.build-glibc/`. It is plain-gcc glibc; no Bionic toolchain. See
   [`overlay.md`](overlay.md).
2. **The patched glibc.** The same host builds glibc from Debian's source at
   the tree's `libc6` version, applying the two patches (Android base +
   dn-policy wiring). The result is the **glibc bundle**: the files that differ
   from stock Debian's `libc6`/`libc-bin`, laid out relative to a prefix. See
   [`overlay.md`](overlay.md), "dn-glibc".
3. **The tree.** `scripts/build/build-core-deb.sh BASE OUT.tar.gz` assembles
   one from an **empty base**: it indexes the mirror, downloads and
   sha256-verifies every package in the pinned list, extracts them, overwrites
   Debian's `libc6`/`libc-bin` payload with the patched bundle, builds the
   `dpkg` database, prunes files no shipped package owns, installs the overlay
   into `RT`, and writes the loader and apt configuration.
4. **The tarball.** `scripts/build/package-prefix.sh TREE --root PREFIX_ROOT
   --name NAME --out OUT.tar.gz` writes `.dn/` and tars the tree.
5. **core-ultra**, optionally, from the core-deb tree
   (`cut-core-ultra.py`), then packaged the same way.

The build inputs to step 3:

| env / arg | meaning |
| --- | --- |
| `BASE` (arg) | the tree to build on; the canonical build uses an **empty** dir |
| `DEB_LIST` | the pinned `name<TAB>version<TAB>arch` list (packages.tsv) |
| `DN_GLIBC_PREFIX` | the glibc bundle's `files/` (the patched files) |
| `DN_OVERLAY` | `build-overlay-glibc.sh`'s output |
| `PREFIX_ROOT` | the absolute path the artifact's files name |
| `DN_PROFILE` | optional; written as `.dn/profile` |
| `DEB_CACHE` | downloaded `.debs`, kept across builds |
| `STAGE_OUT` | optional; keep the built tree (for `cut-core-ultra.py`) |

`package-prefix.sh` stages a copy, runs `pack-prefix.py` on it (the build
invariants and `.dn/`), then `tar --hard-dereference -czf` it (a poor host's
tar cannot recreate hard links). `pack-prefix.py` makes every symlink inside
the tree relative, removes any `ld.so.cache`, creates `home/`, `root`,
`mnt` and `etc/resolv.conf`, records `.dn/baked-paths` and `.dn/packages`, and
writes `.dn/contract` with `root=PREFIX_ROOT`.

## Ship

`scripts/host/ship-prefix.sh ARTIFACT.tar.gz DEST` is the one install path,
run on the host's own shell with only POSIX sh and toybox:

1. **Read the contract without extracting** (`tar -xzOf ARTIFACT
   ./.dn/contract`) and check it: a known contract version, `arch` matching
   `uname -m`, a valid name, the target absent, and `size` fitting the free
   space.
2. **Extract**: `mkdir DEST && tar -xzf`.
3. **Activate**: `DN_INSTDIR=DEST sh DEST/.dn/install.sh`. `.dn/install.sh`
   runs the prefix's own `login.d` hooks and wires the host's session entry
   (`DN_SESSION_SHELL`), if set. **There is no relocation**: the artifact was
   built for `PREFIX_ROOT`, so the host installs it there.
4. **Check that the prefix's shell runs**: `DEST/<entry> -c 'exit 0'` — the
   acceptance test of every prefix.
5. **Complete**: run `.dn/bootstrap.sh` through the prefix's own shell.
6. On any failure, remove `DEST` and report.

The host never edits a file inside the prefix; everything prefix-specific is
the prefix's own scripts.

## Completion

`.dn/bootstrap.sh` restores a shipped core-deb to its full package set, from
the mirror, inside the prefix (the prefix's own bash runs it):

1. `apt-get update`;
2. `apt-get install` of every package named in `.dn/profile` (names only);
3. check `libc6` and `libc-bin` are still held (the patched glibc must
   survive);
4. write `.dn/bootstrapped`, so a second run is a no-op.

The glibc packages are held so `apt` never replaces the patched files; the
version pin is in [`overlay.md`](overlay.md), "the glibc rule".

## Host updates

deb-native's own overlay is not a Debian package, so `apt` cannot update it.
`scripts/host/dn-update.sh` is the constrained primitive that can: copy one
already-built file (or directory) over its fixed location, within an allowlist
(`loader`, `libc`, `trace`, `shell`, ...), never a base package. The runtime
self-derives the prefix from the loader's path, so a straight copy is enough.

## Open items

- **One path per artifact.** With relocation dropped, the artifact is bound to
  `PREFIX_ROOT`. A host must install there. Making the remaining absolute
  config paths (`ld.so.conf`, `apt.conf`) self-derived — or restoring a
  text-only relocation — is the open work to lift this.
- **Artifact distribution**: the build emits the tarball; publishing it, and a
  default source a host can fetch from, is still open.
- **core-ultra**: still cut *from* a full core-deb build rather than assembled
  as its own recipe; sizes and seed set are experiments.
