# The `dn-glibc` prefix: packaging and the GCC lifecycle

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing -- a new spec topic, a new one-off
> investigation, or a new guide -- not just a long addition to what a doc
> already covers.

The next-generation prefix: the runtime's loader is glibc's own, built for the
prefix, and `native/ld-dn.c` is retired. This doc covers how a package gets in
(**packaging**) and how the compiler toolchain fits (**the GCC lifecycle**),
for the prefix this branch is preparing to make installable. The proxy
runtime it replaces: [`ld-dn-runtime.md`](ld-dn-runtime.md) and its
"Alternative: fuse into the loader" section.

Status: design + partial proof (2026-10-03). The mechanism is proven on the
phone (`fused-shim-self-derives-prefix.md`); the packaging and install order
below are what this branch is landing.

## Contents

- [What changes, and what does not](#what-changes-and-what-does-not)
- [The loader needs no code patch](#the-loader-needs-no-code-patch)
- [Fixed paths](#fixed-paths)
- [The package set](#the-package-set)
- [Install order](#install-order)
- [The two interpreter targets](#the-two-interpreter-targets)
- [The GCC lifecycle](#the-gcc-lifecycle)
- [What still needs the tracer](#what-still-needs-the-tracer)

## Related docs

- [`ld-dn-runtime.md`](ld-dn-runtime.md) -- the trampoline being retired; its
  fuse section is this doc's origin.
- [`dl-mechanics.md`](dl-mechanics.md) -- the glibc mechanisms this leans on
  (`ld.so.preload`, `ld.so.cache`), and why they keep the patch small.
- [`package-lifecycle.md`](package-lifecycle.md) -- one package's install
  stages; this doc is the runtime/packaging overlay for the next-gen prefix.
- [`elf-interp-patch.md`](elf-interp-patch.md) -- the `PT_INTERP` edit the
  translate hook makes; only the target string changes here.
- [`../../docs/log/findings/fused-shim-self-derives-prefix.md`](../log/findings/fused-shim-self-derives-prefix.md)
  -- the proof this builds on.
- [`../../docs/log/findings/own-glibc-missing-libc-bin.md`](../log/findings/own-glibc-missing-libc-bin.md)
  -- why `libc-bin` is a custom package here, and the fixed paths below.

## What changes, and what does not

`ld-dn` was a freestanding `PT_INTERP` the kernel ran first, to derive the
prefix and set up the shim + library path. In the `dn-glibc` prefix the kernel
loads glibc's real loader instead, and the prefix is supplied by glibc's own
mechanisms. Concretely:

| concern | `ld-dn` era | `dn-glibc` prefix |
| --- | --- | --- |
| interpreter | `$DN/usr/lib/deb-native/ld-dn` | `$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` |
| prefix derivation | `ld-dn` parses the program's `PT_INTERP` (C2) | baked at glibc configure time (`--prefix=$DN/usr`) |
| shim injection | `ld-dn` builds `LD_PRELOAD` | `$DN/etc/ld.so.preload` |
| library path | `ld-dn` builds `LD_LIBRARY_PATH` | `$DN/usr/etc/ld.so.cache` (ours, via `ldconfig`) |
| env added | `DN_INSTDIR`, `LD_PRELOAD`, `LD_LIBRARY_PATH` | none |

Everything else in the per-package pipeline is unchanged: `dn-translate-deb.sh`
still rewrites `PT_INTERP` and maintainer-script shebangs, `dn-hook-post.sh`
still fixes alternatives/symlinks/launchers, and the `.deb` template crafts
(`dn-package-glibc.sh`, `dn-package-libc-bin.sh`) stay as they are.

## The loader needs no code patch

The plan sketch in `ld-dn-runtime.md` imagined patching `elf/rtld.c` /
`elf/dl-load.c` to derive the prefix and inject the preload. For a
**fixed-prefix** build that is unnecessary: configuring glibc
`--prefix=$DN/usr` already bakes every path the runtime needs
(`elf/rtld.c:1852` reads `SYSCONFDIR "/ld.so.preload"`, the cache/conf come
from `SYSCONFDIR`, and the default search dir is the compiled `libdir`). The
only glibc patches are the ones that already exist for this project:
`set-dirs.patch` (retarget the guest `/etc` etc.) and the Android/seccomp
patch. So "fusing" is a *configuration* plus two standard files, not a fork
of the loader's control flow -- which is what makes it low-risk.

The one thing the loader cannot do is derive the prefix at runtime; it does
not need to, because the prefix is fixed. (The shim, which must also know the
prefix, derives it from its own load path with `dladdr` --
`fused-shim-self-derives-prefix.md`.)

## Fixed paths

Read from the built binaries (`own-glibc-missing-libc-bin.md`); `<prefix>` is
`$DN`:

| file | baked path | written by | read by |
| --- | --- | --- | --- |
| `ld.so.preload` | `<prefix>/etc/ld.so.preload` | install | loader |
| `ld.so.cache` | `<prefix>/usr/etc/ld.so.cache` | our `ldconfig` | loader |
| `ld.so.conf` | `<prefix>/usr/etc/ld.so.conf` | install/`ldconfig` | `ldconfig` |
| loader | `<prefix>/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` | `libc6` | kernel (`PT_INTERP`) |

The `etc` vs `usr/etc` split is the one non-obvious wiring point: glibc's
`SYSCONFDIR` is `<prefix>/usr/etc` (because configure is `--prefix=<prefix>/usr`),
while `set-dirs.patch` retargets the guest `/etc` for `ld.so.preload`. Debian's
own `libc-bin` installs `ld.so.conf` to the guest `etc`, so the conf must be
placed at `<prefix>/usr/etc/` for our `ldconfig` to read it.

## The package set

- **Ours (shipped prebuilt):** `libc6` (loader + shared libs) and `libc-bin`
  (`ldconfig`, `ldd`, ...). Built by `scripts/bootstrap/dn-package-glibc.sh`
  and `dn-package-libc-bin.sh`, `dpkg -i`'d and held.
- **Debian, unmodified, version-matched:** `libc6-dev`, `libc-dev-bin`,
  `locales`, `libc-l10n` -- only version-sensitive, so they install straight
  from the archive once our `libc6` carries Debian's exact version string
  (`libc6-dev-gap-closed.md`).
- **Everything else:** ordinary Debian `.deb`s through the translate pipeline.

The aarch64 target is assumed throughout (the loader name and multiarch dir
are hardcoded today, as they already are in `dn-translate-deb.sh`).

## Install order

The order that makes the prefix self-consistent:

1. `dn-debian-index.sh`, then bootstrap the base set as today.
2. Install our prebuilt `libc6` + `libc-bin` (the loader + `ldconfig`). Until
   this lands, `PT_INTERP` has no valid target -- see the bootstrap note below.
3. Place `ld.so.preload` (shim) at `<prefix>/etc/` and `ld.so.conf` at
   `<prefix>/usr/etc/`; run our `ldconfig` to build `<prefix>/usr/etc/ld.so.cache`.
4. Install `libc6-dev`/`libc-dev-bin` (headers, crt, linker scripts) and the
   toolchain (`binutils`, `cpp`, `gcc-14`, `libgcc-s1`, `libstdc++6`).
5. Post-hook (`dn-fix-gcc-specs.sh`) writes gcc's `specs` to link against the
   fused loader.

**Bootstrap note.** A fresh prefix has no compiler, but our `libc6` must exist
before any translated Debian binary can run -- so `libc6`/`libc-bin` are a
*prebuilt artifact*, installed in step 2, not built in-prefix. The prebuilt
glibc is itself compiled outside the fresh prefix (an existing prefix's `gcc`,
`third_party/glibc-android-patches/README.md`, or Termux's clang for the very
first build); the prefix never self-hosts glibc.

## The two interpreter targets

Two strings change in the shipped scripts; both go from `ld-dn` to the loader:

- `scripts/install/dn-translate-deb.sh` (`LD`): every translated glibc ELF's
  `PT_INTERP` becomes the fused loader, so the kernel loads it. The existing
  match (`*/ld-linux-aarch64.so.1`) is unchanged -- only the target.
- `scripts/install/dn-fix-gcc-specs.sh` (`LDDN`): gcc's `*link` spec writes the
  fused loader as `-dynamic-linker`, so a binary `gcc` links itself gets a
  resolvable `PT_INTERP` (the `ld-dn` fix at
  `findings/gcc-hello-pt-interp-gap.md`, retargeted).

Both are also the transition seam: pointing them back at
`$DN/usr/lib/deb-native/ld-dn` restores the trampoline pipeline while the
fused path is still being brought up.

## The GCC lifecycle

GCC plays two roles; only the first is a package-lifecycle concern.

**(a) Consumer -- the compiler users run.** It installs from Debian's archive
like any package (`gcc-14`, `cpp`, `binutils`, `libgcc-s1`, `libstdc++6`,
`gcc-14-base`), translated normally. Its two needs:

- *compile*: headers, crt objects and linker scripts from `libc6-dev` under
  `<prefix>/usr/include` and `<prefix>/usr/lib/aarch64-linux-gnu`.
- *link*: a resolvable `-dynamic-linker` -- the `specs` file written by the
  post-hook. GCC reads an optional `specs` beside its `libgcc.a`
  (`gcc -print-libgcc-file-name`'s directory) and overrides its built-in
  `GLIBC_DYNAMIC_LINKER` with no gcc/binutils rebuild.

So `gcc -o prog prog.c` emits `PT_INTERP` = the fused loader, and `./prog` runs
env-free, with the shim (from `ld.so.preload`) and the prefix libs (from
`ld.so.cache`). No `dn-run`, no `ld-dn`.

**(b) Bootstrap tool -- what builds our glibc.** This is the chicken-and-egg
from the install order: glibc needs a compiler, but the compiler needs glibc.
It is broken by shipping `libc6`/`libc-bin` prebuilt (step 2), and by building
that prebuilt glibc with a compiler *outside* the fresh prefix. The prefix's
own `gcc` is never used to build its own glibc; it is purely the consumer above.

## What still needs the tracer

The fused loader only covers **glibc-dynamic** ELFs. Static binaries, Bionic
binaries, and programs making raw syscalls never reach the loader and still
need `dn-run` + `dn-trace` (`make-launchers.sh` classification,
[`tracer.md`](tracer.md), [`syscall-boundary.md`](syscall-boundary.md)). The
fused loader removes `ld-dn`, not the tracer.
