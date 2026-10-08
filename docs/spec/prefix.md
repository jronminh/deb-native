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
- [Completion](#boot)
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

Every glibc ELF keeps the interpreter Debian gave it
(`/lib/ld-linux-aarch64.so.1`); the kernel never resolves that path, because
every exec goes through [`overlay.md`](overlay.md)'s exec gate, which runs a
glibc-dynamic program through `RT/ld.so` itself. A package is therefore
installed intact, and **nothing repoints a `PT_INTERP` after the build**.

The prefix root (`TREE`) is **not baked**: `dn-trace` derives it from its own
location (`TREE/usr/lib/deb-native/dn-trace`), the loader self-derives it from
its path too, and the tree's config files (`usr/etc/ld.so.conf`,
`etc/apt/apt.conf.d/`) name **guest** paths, which dn-policy maps into whatever
tree dn-trace booted. The build still passes a `PREFIX_ROOT` — to *assert*
nothing names it — but the artifact is relocatable: the host extracts it at any
`DEST` and installs there.

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
│   ├── baked-paths     files naming the build path (empty: relocatable)
│   └── install.sh      /system/bin/sh, run by the host's shell: activate
├── home/
├── root -> home
├── mnt -> ../mnt
├── etc/resolv.conf -> ../../app/etc/resolv.conf
└── usr/ etc/ var/ ...  the tree, as built
```

`.dn/packages` is one `package<TAB>version<TAB>arch` per line.
`.dn/baked-paths` records any file the build path (`PREFIX_ROOT`) is written
into, one per line, fields tab-separated, paths relative to the root.  A
relocatable artifact names its root nowhere, so the list is **empty**; a
non-empty `text` list means some file still names the build path, and
`pack-prefix.py` fails the build on an `elf` that does.

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
| `loader` | yes | the loader, relative to the root |
| `install` | yes | the activation script, relative to the root |
| `entry` | yes | the command that boots / opens a session; a full command line, run by the host with the prefix root as the working directory |
| `size` | no | the extracted size in MiB, for a free-space check |

Example (core-deb):

```
# dn prefix contract
contract=1
name=core-deb
desc=core-deb: core-ultra plus apt and dpkg
version=0.7.1-dev
arch=aarch64
loader=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
install=.dn/install.sh
entry=usr/lib/deb-native/dn-trace -- /usr/bin/bash /usr/lib/deb-native/init.sh
size=200
```

## Build

A build host: an `arm64` Linux machine with `gcc`, `make`, `python3`,
`dpkg-deb`, `wget` or `curl`, `xz`. Nothing here runs inside a prefix or on
the target.

1. **The overlay.** `scripts/build/build-overlay-glibc.sh` builds `dn-trace`
   from `src/tracer/` into `src/.build-glibc/` — the whole overlay, one file:
   the syscall catalog `src/syscalls.tsv` is embedded in it at build time.
   dn-trace is **static and self-contained** (its talloc
   is vendored in `src/tracer/talloc/`), linked against the Android-patched
   glibc's `libc.a` (`DN_GLIBC_LIBC_DIR`): Android's zygote seccomp kills a
   stock-glibc program at startup, so the patched libc is required. A poor
   host then starts dn-trace with a bare exec, outside the runtime. See
   [`overlay.md`](overlay.md).
2. **The patched glibc.** The same host builds glibc from Debian's source at
   the tree's `libc6` version, applying the two patches (Android base +
   dn-policy wiring), then packages the result as two real Debian packages —
   `libc6` and `libc-bin`, at `<Debian version>+dn1`
   (`scripts/glibc/dn-package-glibc.sh`, `dn-package-libc-bin.sh`). This is the
   **glibc bundle**: `libc6.deb` + `libc-bin.deb`. See
   [`overlay.md`](overlay.md), "dn-glibc".
3. **The tree.** `scripts/build/build-core-deb.sh BASE OUT.tar.gz` assembles
   one from an **empty base**: it indexes the mirror, downloads and
   sha256-verifies every package in the pinned list (using our `libc6`/
   `libc-bin` instead of the mirror's), extracts them, builds the `dpkg`
   database as `dpkg --unpack` would leave it (status `unpacked`, with each package's
   `Conffiles`, file lists, and every control file: `md5sums`, `conffiles`,
   maintainer scripts, triggers; `<pkg>:<arch>` for a `Multi-Arch: same`
   package), prunes files no shipped package owns, installs the overlay into
   `RT`, builds the **local repo** (`RT/repo`, its `Release` dated and
   carrying the index hashes) with the patched glibc packages, ships the
   prefix's init and the loader, and writes the apt configuration and
   `etc/ca-certificates.conf` (every certificate enabled: the tree has no
   debconf to ask). The bootstrap then builds the CA bundle.
4. **The tarball.** `scripts/build/package-prefix.sh TREE --root PREFIX_ROOT
   --name NAME --out OUT.tar.gz` writes `.dn/` and tars the tree.
5. **core-ultra**, optionally, from the core-deb tree
   (`cut-core-ultra.py`), then packaged the same way.

The build inputs to step 3:

| env / arg | meaning |
| --- | --- |
| `BASE` (arg) | the tree to build on; the canonical build uses an **empty** dir |
| `DEB_LIST` | the pinned `name<TAB>version<TAB>arch` list (packages.tsv) |
| `DN_GLIBC_PREFIX` | the glibc bundle: a dir with `libc6.deb` + `libc-bin.deb` |
| `DN_OVERLAY` | `build-overlay-glibc.sh`'s output |
| `PREFIX_ROOT` | the path the build assumes; asserted not to be named (the artifact is relocatable) |
| `DN_PROFILE` | optional; written as `.dn/profile` |
| `DEB_CACHE` | downloaded `.debs`, kept across builds |
| `STAGE_OUT` | optional; keep the built tree (for `cut-core-ultra.py`) |

`package-prefix.sh` stages a copy, runs `pack-prefix.py` on it (the build
invariants and `.dn/`), then `tar --hard-dereference -czf` it (a poor host's
tar cannot recreate hard links). `pack-prefix.py` makes every symlink inside
the tree relative, removes any `ld.so.cache`, creates `home/`, `root`,
`mnt` and `etc/resolv.conf`, records `.dn/baked-paths` and `.dn/packages`, and
writes `.dn/contract`. It fails the build if any file names `PREFIX_ROOT`.

## Ship

`scripts/host/ship-prefix.sh ARTIFACT.tar.gz DEST` is the one install path,
run on the host's own shell with only POSIX sh and toybox:

1. **Read the contract without extracting** (`tar -xzOf ARTIFACT
   ./.dn/contract`) and check it: a known contract version, `arch` matching
   `uname -m`, a valid name, the target absent, and `size` fitting the free
   space. A current artifact names no root; one that still carries `root=` only
   installs at that path.
2. **Extract**: `mkdir DEST && tar -xzf`.
3. **Activate**: `DN_INSTDIR=DEST sh DEST/.dn/install.sh`. It wires the host's
   session entry (`DN_SESSION_SHELL`), if set, to the contract's `entry`. No
   tree program runs here.
4. **Check the tree runs**: start it through `dn-trace` for a trivial command
   (`DEST/usr/lib/deb-native/dn-trace -- /usr/bin/bash -c 'exit 0'`). This is
   the acceptance test.
5. On any failure, remove `DEST` and report.

The host never edits a file inside the prefix; everything prefix-specific is
the prefix's own scripts.

## Boot

The host boots the tree by running the contract's `entry` from the prefix
root. `dn-trace` **derives** the tree from its own location
(`TREE/usr/lib/deb-native/dn-trace`) and takes its runtime loader from the
fixed path `TREE/<loader>`, so it never parses the contract and names no root;
an optional `-- PROGRAM ARGS...` is in guest paths and defaults to the init:

```
dn-trace [TREE LOADER] [-- PROGRAM ARGS...]
```

(Two absolute `TREE LOADER` arguments still override the derivation, for
starting a tree by absolute path or for tests.)

It becomes the tree's root process with TREE as the guest root (TREE is also
bound to itself, so a host path into the tree stays valid in the guest), forks
a child that installs the one shared filter, and execs the prefix's init
(`RT/init.sh`). Its own temp files live in `TREE/tmp`. init sets
the environment and, on a first boot (no `.dn/bootstrapped`), completes the
prefix itself. The build cannot run a package's scripts, so the shipped
packages are *unpacked*, not configured (as after debootstrap's first stage):
the first boot runs each one's `preinst install` (base-passwd's first: it
writes `/etc/passwd` and `/etc/group`), then `dpkg --configure -a`, under fake
root. Then it restores `.dn/profile` from the mirror — `apt-get update`,
`apt-get install` of the profile, the CA bundle — and writes the
`bootstrapped` marker; a later boot skips straight to the command. It ends by exec'ing the command it was given, or an interactive
shell.

The glibc packages are held and pinned, so `apt` never replaces the patched
files ([`overlay.md`](overlay.md), "the glibc rule").

## Host updates

deb-native's own overlay is not a Debian package, so `apt` cannot update it.
`scripts/host/dn-update.sh` is the constrained primitive that can: copy one
already-built file (or directory) over its fixed location, within an allowlist
(`loader`, `libc`, `trace`, `shell`, ...), never a base package. The runtime
self-derives the prefix from the loader's path, so a straight copy is enough.

## Open items

- **Artifact distribution**: both tarballs are published to the rolling
  `prefix` release, and `scripts/host/install-from-release.sh` fetches one and
  ships it to a chosen `DEST`. Verifying that fetch and a relocated boot on a
  real poor host is still open.
- **core-ultra**: still cut *from* a full core-deb build rather than assembled
  as its own recipe; sizes and seed set are experiments.
