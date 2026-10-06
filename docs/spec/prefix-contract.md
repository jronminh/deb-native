# The prefix contract: `.dn/`

<!-- template: templates/docs.template.md -->

Every prefix artifact carries a `.dn/` directory at its root: a **contract**
file the host reads without running any code, a map of every byte range that
names the build path, and an optional **relocation script** the host runs
with its own shell. Installing a prefix needs nothing but a POSIX shell and
`tar`, `dd`, `sed` (Android's `mksh` + toybox is enough): read the contract,
extract, and -- only when the prefix landed somewhere other than where it was
built -- run the relocation script, which **patches the binaries' bytes
directly** at offsets the build recorded. No ELF tool and no program of the
prefix runs during an install. Status: design, verified by an experiment on
core-deb and core-ultra (`prefix-layers.md`); the build does not emit `.dn/` yet and no host reads it.

## Contents

- [The model](#the-model)
- [Layout](#layout)
- [The contract file](#the-contract-file)
- [What the host does](#what-the-host-does)
- [The relocation script](#the-relocation-script)
- [Why patching bytes is safe here](#why-patching-bytes-is-safe-here)
- [Build invariants](#build-invariants)
- [Verified](#verified)
- [One ship path](#one-ship-path)
- [Open items](#open-items)

## Related docs

- [`../../MODULARIZE.md`](../../MODULARIZE.md) -- Build vs Ship and the
  prefix artifact this contract is part of.
- [`../reference/elf-interp-patch.md`](../reference/elf-interp-patch.md) --
  how the kernel reads `PT_INTERP` (from the file, never mapped), which is
  what makes an in-place byte patch valid.
- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) -- the prefix's own loader, which
  derives the live prefix from its own path.
- [`prefix-layers.md`](prefix-layers.md) -- core-ultra and core-deb, the
  artifacts this contract describes.

## The model

An artifact is a **self-contained prefix**: everything it needs is inside it,
including `home/`, `root -> home` and the `mnt -> ../mnt` link, all as
relative paths. Extracted where it was built, it runs as is -- installing it
is `tar x`.

It is **not position-independent**: every glibc ELF names the prefix's loader
by absolute path in `PT_INTERP`, and scripts and config name the prefix by
absolute path (shebangs, wrappers, `etc/ld.so.preload`). The kernel requires
both to be absolute. Moving a prefix therefore means rewriting those paths --
**relocation**, the one and only reason an install ever runs code.

Relocation is split so that the hard part happens where tools are:

- **at build time**, with a toolchain and `patchelf`, every glibc ELF's
  `PT_INTERP` is given a fixed **capacity** (256 bytes), and the build records
  each one's **file offset**;
- **at install time**, the host's shell overwrites those bytes with `dd` and
  rewrites the text files with `sed`. Nothing reads or parses an ELF.

So the host:

1. reads the contract (data, no code);
2. extracts the artifact;
3. runs the prefix's relocation script with its own shell, when the contract
   names one;
4. runs the artifact's `install` script (host-side activation) and its
   `bootstrap` script through the prefix's own shell (completion), when the
   contract names them. What install and bootstrap do is below.

What a prefix needs beyond being in place is not part of installing it.
Data that belongs to the host -- the phone's storage, the device's DNS -- is
kept by the host next to the prefixes and reached through relative links the
artifact carries (`mnt -> ../mnt`, `etc/resolv.conf ->
../../app/etc/resolv.conf`), so it is always current and the host never
writes into a prefix. What the prefix itself runs (services) belongs to its
own `boot.d` / `login.d` hooks.

## Layout

```
<prefix>/
├── .dn/
│   ├── contract        data: key=value, read by the host without running code
│   ├── baked-paths     every place the build path is written (by the build)
│   ├── packages        the Debian packages the prefix contains (prefix-layers.md)
│   ├── profile         the packages a bootstrap restores from the mirror
│   ├── relocate.sh     optional: POSIX sh, run by the host's shell
│   ├── install.sh      optional: /system/bin/sh, run by the host's shell, activates the prefix
│   └── bootstrap.sh    optional: run by the prefix's own shell, completes it
├── home/               empty
├── root -> home
├── mnt -> ../mnt       the host's storage when it has a sibling mnt/; dangling otherwise
├── etc/resolv.conf -> ../../app/etc/resolv.conf    the host's DNS, kept current by the host
└── usr/ etc/ var/ ...  the prefix, as built
```

`.dn/` sits outside the FHS tree on purpose: it is the prefix's interface to
its host, not part of the userland. It is unrelated to
`var/lib/deb-native/prefix-manifest.tsv` (the overlay components and their
sha256, for `dn-update`).

`.dn/baked-paths` lists every file that carries the build path, one per line,
fields separated by a tab, paths relative to the root:

```
elf	usr/bin/bash	1640192	256
elf	usr/lib/aarch64-linux-gnu/libcap.so.2.75	568	256
text	etc/ld.so.preload
text	usr/bin/zcat
```

- `elf FILE OFFSET CAPACITY` -- a glibc ELF whose `PT_INTERP` string starts at
  byte `OFFSET` and has `CAPACITY` bytes (`p_filesz`, the NUL included);
- `text FILE` -- a file whose content names the build path (shebangs,
  wrappers, config).

The build writes it, so an install never scans the tree (the 0.7.1-dev core
prefix: 181 `elf`, 139 `text`, about 5000 files in all).

## The contract file

`.dn/contract` is plain text a POSIX shell parses with `while read`:

- one `key=value` per line, split at the first `=`; no quoting or escapes;
- blank lines and lines starting with `#` are ignored;
- keys are lowercase; a repeated key keeps the last value;
- an unknown key is ignored with a warning, so a newer artifact still
  installs on an older host as long as `contract` allows it.

| key | required | meaning |
| --- | --- | --- |
| `contract` | yes | the contract version (integer). A host refuses a version it does not know. This doc is version `1`. |
| `name` | yes | the default directory name and the name shown by the host. |
| `desc` | no | one line describing what the prefix is for. |
| `version` | no | the prefix's own version (e.g. `core/VERSION` for core-ultra and core-deb). |
| `arch` | yes | the CPU architecture (`uname -m`, e.g. `aarch64`). |
| `root` | yes | the absolute directory the prefix's files currently name: the build path in an artifact, the install path once relocated. |
| `loader` | with `relocate` | the prefix's loader, relative to the root; `root/loader` is the string every `elf` entry holds. |
| `relocate` | no | the relocation script, relative to the root. Absent: the prefix installs only at `root`. |
| `install` | no | the activation script, relative to the root; run by the host's `/system/bin/sh` after relocation. Absent: nothing to activate. |
| `bootstrap` | no | the completion script, relative to the root; run by the prefix's own shell after `install`. Absent: the prefix is complete as shipped. |
| `entry` | yes | the command, relative to the root, that opens an interactive session. |
| `size` | no | the extracted size in MiB, for a free-space check. |

Example (core-deb, built in Termux):

```
# dn prefix contract
contract=1
name=core-deb
desc=core-deb: core-ultra plus apt, dpkg and the package translation hooks
version=0.7.1-dev
arch=aarch64
root=/data/data/com.termux/files/deb-native
loader=usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
relocate=.dn/relocate.sh
install=.dn/install.sh
bootstrap=.dn/bootstrap.sh
entry=usr/bin/dn-shell -i
size=200
```

## What the host does

`install NAME ARTIFACT`, on any host, with a POSIX shell, `tar`, `dd` and
`sed`:

1. **Read the contract without extracting**:
   `tar -xzOf ARTIFACT ./.dn/contract` (toybox `tar` supports `-z` and `-O`).
   **Check**, and refuse before writing anything: `contract` is a version it
   knows, `arch` equals `uname -m`, the name is valid, the target directory
   `D` does not exist, `size` fits the free space, and -- when the contract
   has no `relocate` -- `D` is `root` (compared after resolving symlinks:
   `/data/user/0/...` and `/data/data/...` are the same directory).
2. **Extract**: `mkdir D && tar -xzf ARTIFACT -C D`.
3. **Relocate**, when the contract names a script: `sh D/<relocate>` with
   `DN_INSTDIR=D`, run by the host's own shell. The script returns at once
   when `root` already is `D`.
4. **Check that the prefix's shell runs**: `D/<entry> -c 'exit 0'` (the
   entry is a shell). It is the acceptance test of every prefix: installed
   means the shell works.
5. **Activate**, when the contract names an `install` script: `sh D/<install>`
   with `DN_INSTDIR=D`, run by the host's own shell (on Android,
   `/system/bin/sh`). This is the host-side integration that makes the prefix
   enterable (a session entry, a launcher); the artifact carries it, so the
   host needs no per-target logic of its own.
6. **Complete**, when the contract names a `bootstrap` script: run it with the
   prefix's own shell, `DN_INSTDIR=D "$D/usr/bin/bash" "$D/<bootstrap>"`. The
   prefix installs what `.dn/profile` lists from the mirror and writes
   `.dn/bootstrapped`; a second run is a no-op. This is the prefix's own
   logic, not the host's, and the one step that needs the network.
7. **On any failure** in 2-6, remove `D` and report it with the step's output.
   On success the prefix is ready to use; the host may integrate it on its own
   side (a prefix list, a login entry) without writing into it.

The host reads `name`, `desc` and `entry` again whenever it lists or enters
a prefix; it never needs the prefix to run for that.

## The relocation script

`.dn/relocate.sh` is the prefix's own logic, run by the host's shell before
any program of the prefix can start. It is written to the smallest common
environment: **POSIX `sh`** (no bash or mksh extensions) and **only `printf`,
`dd`, `sed`, `wc`**, which toybox and coreutils both have. Input:
`DN_INSTDIR` (`D`), `.dn/contract`, `.dn/baked-paths`. Exit status is the
result; it is idempotent.

When `root` (`R`) in the contract differs from `D`, with `OLD = R/<loader>`
and `NEW = D/<loader>`:

1. **ELF interpreters, by byte patch.** For each `elf FILE OFFSET CAPACITY`:
   - refuse if `NEW` plus its NUL does not fit `CAPACITY`;
   - **read the bytes at `OFFSET` and refuse unless they are `OLD`** -- a
     mismatch means the file changed since the build, and writing would
     corrupt it;
   - write `NEW` and a NUL over them:

     ```
     cur=$(dd if=FILE bs=1 skip=OFFSET count=${#OLD} 2>/dev/null)
     [ "$cur" = "$OLD" ] || exit 1
     printf '%s\000' "$NEW" | dd of=FILE bs=1 seek=OFFSET conv=notrunc 2>/dev/null
     ```
2. **Text files.** For each `text FILE`, `sed -i` replacing `R` with `D`. `D`
   may contain `R` (`R` = `.../files`, `D` = `.../files/core`), so
   occurrences of `D` are protected first and a rerun never produces
   `.../core/core`:

   ```
   sed -i "s|$D|@@DN@@|g; s|$R|$D|g; s|@@DN@@|$D|g" FILE
   ```
3. **Record it**: set `root=D` in `.dn/contract`.

A refusal leaves the files patched so far as they are; at install time the
host removes `D` on any failure, so nothing half-relocated survives.

From then on the prefix runs normally: the kernel reads each new
`PT_INTERP`, the loader reads the new `etc/ld.so.preload`, and the shim and
glibc self-derive the prefix at run time.

## Why patching bytes is safe here

- **The kernel reads `PT_INTERP` from the file**, through
  `p_offset`/`p_filesz`, at `execve()`, and never maps it
  (`elf-interp-patch.md`). Changing those bytes changes nothing else in the
  ELF: no header, no segment, no layout.
- **It needs only a terminated string.** The kernel requires the last byte of
  the `p_filesz` range to be NUL (the capacity's last byte stays NUL) and
  opens the path up to the first NUL; anything after it is ignored.
- **No other binary byte names the prefix.** Measured on core-deb: no
  ELF carries the build path anywhere but in `PT_INTERP` (the glibc and the
  shim derive the prefix at run time), so the offsets are the whole job.
- **Every write is checked first.** The offset is trusted only while the
  bytes there are still the old loader path.

The cost is the capacity: the path to the loader at the install site must be
shorter than 256 bytes (core-deb in the app, at
`/data/data/org.dn.shell/files/core-deb/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`,
is 86).

## Build invariants

The build guarantees these, so relocation stays the steps above:

1. **Every glibc ELF's `PT_INTERP` has the full capacity** (256 bytes): the
   build sets a 255-character placeholder with `patchelf` (the only point
   where its layout rewrite runs, and where the result can be checked), then
   writes the real loader path and a NUL at its start, and records the
   offset.
2. **Only ELFs that use the prefix's glibc loader are touched.** `dn-run` and
   `dn-trace` are Bionic (`/system/bin/linker64`) and must keep that
   interpreter; repointing them breaks every launcher.
3. **No `ld.so.cache`** in the artifact. The loader derives its library
   directories from its own path; a cache holds absolute paths in a binary
   format no text rewrite fixes, and the static `ldconfig` cannot derive the
   prefix (`deploy.md`, Open items).
4. **Every symlink inside the prefix is relative.** A relative link never
   needs relocating. Today the apt post-hook runs `normalize-symlinks.sh`
   before `make-launchers.sh`, which leaves 44 absolute links in
   `usr/lib/deb-native/bin/`; the build normalizes after the last step that
   makes links.
5. **`.dn/baked-paths` is complete**, and **no binary carries the build path
   outside `PT_INTERP`**; the build checks both and fails otherwise.
6. **`home/`, `root -> home` and `mnt -> ../mnt` are in the artifact**, all
   relative.
7. **`etc/resolv.conf` is the relative link `../../app/etc/resolv.conf`**,
   not a file: every host keeps its DNS servers in `app/etc/resolv.conf` beside
   its prefixes (the dn-shell app from the device's active network; a Termux
   host from Termux's own `resolv.conf`). glibc rereads the file when it
   changes, so a new network reaches every prefix with nothing run inside it.
   With no such file the link dangles and names do not resolve; nothing hangs.
   Verified: a host writing `nameserver 8.8.8.8` there let `getent ahosts`
   resolve in core-deb, and changing the file changed the next lookup with no
   action in the prefix.

The build is the separate stage that ends in the tarball: a toolchain
produces the components, they are assembled into a prefix wherever the build
runs, and the package step applies the invariants, writes `.dn/` (`root` =
that build path) and tars the tree. The artifact is not relocated at build
time.

## Verified

On core-deb (0.7.1-dev, built for `/data/data/org.dn.shell/files/core`),
installed with `env -i PATH=/system/bin /system/bin/sh` (Android `mksh` +
toybox only) into a different directory whose loader path is 190 bytes:

- the contract read with `tar -xzOf`, the tree extracted, `relocate.sh` run:
  181 ELFs and 139 text files in 2-4 s; a rerun changes nothing;
- run straight from `mksh`, no loader invoked by hand: `bash`, `dpkg`,
  `apt`; in a `dn-shell` session: `dpkg -l` (61 packages), `perl` through
  its launcher and `dn-run`, the fake-root identity, a `#!.../dn-shell`
  script (`zcat`), `/mnt` through `mnt -> ../mnt`;
- no file left naming the build path;
- a file changed after the build (bytes at its `PT_INTERP` offset
  overwritten) is refused, not patched.

Not exercised: `apt install` over the network, `dn-trace`, the dn-shell app
itself.

## One ship path

Build and ship are separate (`MODULARIZE.md`, "Build vs Ship"), so there is
exactly **one way to ship a prefix**, the host steps above, on every
target:

- the dn-shell app runs them on its bundled asset (`dn-prefix install`);
- Termux's `install.sh` runs them on a downloaded artifact. It never builds;
  with no artifact it stops and says so.

## Open items

- **The build does not emit `.dn/` yet**: `package-prefix.sh` needs the
  invariants above, the `baked-paths` scan with offsets, the contract and
  `relocate.sh` (from `core/`).
- **Termux still has two install paths**: `install.sh`'s default mode
  bootstraps in place (a build that is also the install), and its
  `DN_PREFIX_IMAGE` mode extracts and runs `dn-finish.sh` with the host's
  shell and skips the login wiring. Both become the one ship path; the
  in-place bootstrap moves to the build stage. `dn-finish.sh`'s steps are
  build or apt-hook steps, none of them needed at install.
