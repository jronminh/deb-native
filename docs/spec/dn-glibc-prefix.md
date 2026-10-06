# The `dn-glibc` prefix: packaging and the GCC lifecycle

<!-- template: templates/docs.template.md -->

The next-generation prefix: the runtime's loader is glibc's own, built for the
prefix, and `core/native/ld-dn.c` is retired. This doc covers how a package gets in
(**packaging**) and how the compiler toolchain fits (**the GCC lifecycle**),
for the prefix this branch is preparing to make installable. The proxy
runtime it replaces: `ld-dn-runtime.md` and its
"Alternative: fuse into the loader" section.

Status: shipped (0.6.0+s.1). The mechanism is proven on the phone
(`fused-shim-self-derives-prefix.md`); the packaging, install order and
run-time prefix self-derivation below are what shipped.

## Contents

- [What changes, and what does not](#what-changes-and-what-does-not)
- [One small loader patch](#one-small-loader-patch)
- [Fixed paths](#fixed-paths)
- [Runtime prefix self-derivation](#runtime-prefix-self-derivation)
- [The package set](#the-package-set)
- [Install order](#install-order)
- [The two interpreter targets](#the-two-interpreter-targets)
- [The GCC lifecycle](#the-gcc-lifecycle)
- [What still needs the tracer](#what-still-needs-the-tracer)

## Related docs

- `ld-dn-runtime.md` -- the trampoline being retired; its
  fuse section is this doc's origin.
- [`dl-mechanics.md`](../reference/dl-mechanics.md) -- the glibc mechanisms this leans on
  (`ld.so.preload`, `ld.so.cache`), and why they keep the patch small.
- [`package-lifecycle.md`](package-lifecycle.md) -- one package's install
  stages; this doc is the runtime/packaging overlay for the next-gen prefix.
- [`elf-interp-patch.md`](../reference/elf-interp-patch.md) -- the `PT_INTERP` edit the
  translate hook makes; only the target string changes here.
- `../../docs/log/findings/fused-shim-self-derives-prefix.md`
  -- the proof this builds on.
- `../../docs/log/findings/own-glibc-missing-libc-bin.md`
  -- why `libc-bin` is a custom package here, and the fixed paths below.
- `../../docs/log/findings/glibc-patch-swap-set.md`
  -- the exact 10 files the patch affects; the set [Runtime prefix
  self-derivation](#runtime-prefix-self-derivation) makes prefix-agnostic.

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

## One small loader patch

The plan sketch in `ld-dn-runtime.md` imagined patching `elf/rtld.c` /
`elf/dl-load.c` to derive the prefix and inject the preload. For a
**fixed-prefix** build that is mostly unnecessary: configuring glibc
`--prefix=$DN/usr` already bakes every path the runtime needs
(`elf/rtld.c` reads `SYSCONFDIR "/ld.so.preload"`, the cache/conf come from
`SYSCONFDIR`, and the default search dir is the compiled `libdir`), so the
prefix-derivation and preload-injection patches are not needed, and the shim
derives the prefix itself (`dladdr`, `fused-shim-self-derives-prefix.md`).
"Fusing" is therefore mostly *configuration* plus two standard files, not a
fork of the loader's control flow.

**The one exception (2026-10-03):** the loader must **ignore the inherited
`LD_PRELOAD`**. `ld-dn` used to sanitize the environment for every binary --
it replaced `LD_PRELOAD` with its own shim -- so a host `LD_PRELOAD`
(Termux's `libtermux-exec-ld-preload.so`, set in every Termux shell) never
reached a prefix program. The fused loader does not sanitize, and that host
library is built for another glibc, so it aborts every prefix program at
startup (found migrating `.dn`). The fix is a small `elf/rtld.c` hunk (part
of `dn-glibc-android.patch`): skip the `state.preloadlist` (`LD_PRELOAD`)
source in `dl_main`, keeping `--preload` and the `ld.so.preload` file. Why
this is safe: the prefix's shim is delivered by `ld.so.preload`, not the env,
so dropping `LD_PRELOAD` costs nothing and restores `ld-dn`'s sanitization.
`core/native/dn-run.c` was updated to match: it no longer injects the shim via
`LD_PRELOAD`, only drops the inherited host preload.

This fixed-prefix build needs no runtime derivation: every path is
compile-time, and the shim arrives via `ld.so.preload`. The self-deriving
variant that would make one build fit every prefix is a separate step --
[Runtime prefix self-derivation](#runtime-prefix-self-derivation).

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

## Runtime prefix self-derivation

Everything above assumes a **fixed** prefix: glibc is configured
`--prefix=<prefix>/usr`, so `<prefix>` is baked into the binaries. The Android
patch touches only **10 files** (`../log/findings/glibc-patch-swap-set.md`),
but a prebuilt set still fits exactly one prefix -- moving it, or installing
under a different one, needs a rebuild. This section designs the fix: make
those files **derive the prefix at run time**, so one prebuilt set works under
any prefix, and a Debian base update never forces a glibc rebuild.

Status: shipped (0.6.0+s.1). Implemented as **one global live prefix** in
`sysdeps/generic/dn-prefix.h` + `elf/rtld.c` + `elf/dl-cache.c` +
`elf/ldconfig.c` -- not per-file `dladdr` as first sketched below. The
static `ldconfig` is the one exception (see [`deploy.md`](deploy.md),
Open items).

### How the live prefix is cut

The loader runs before the link maps (and libc) exist, so it cannot call
`dladdr` -- but it does not need to: it already holds its own path, taken
from the main executable's `PT_INTERP`, in `_dl_rtld_map.l_name`
(`elf/rtld.c`). `ld.so.preload` is read later and the cache only on first
lookup, so the path is available before either is used.

Shipped as **one global**, cut by the loader and shared:

- `elf/rtld.c` cuts the live prefix from `_dl_rtld_map.l_name` once, before
  the search paths are built, and exports it as the weak-linked
  `__dn_prefix_get()`.
- `sysdeps/generic/dn-prefix.h` declares `__dn_prefix_get()` (weak) and
  `__dn_build(dst, sz, "/guest/path")`, which prepends the live prefix.
  When the live prefix is NULL (a static link), `__dn_build` leaves the
  guest path as-is.
- libc's own internal opens (`elf/dl-load.c`, ...) and `elf/dl-cache.c`'s
  cache path use the same global; `elf/ldconfig.c` derives its
  cache/conf/libdirs/aux paths from it.

Match requires a path-component boundary, so `.dn` does not match `.dn12`.
The compiled `@TERMUX_PREFIX@` stays the fallback when derivation yields
nothing.

### Patch shape

1. One global live prefix (`__dn_prefix_get`), cut once in `elf/rtld.c`
   and declared in `sysdeps/generic/dn-prefix.h`.
2. Replace the baked `"@TERMUX_PREFIX@/..."` literals with strings built at
   run time via `__dn_build(...)`.
3. In the loader, `preload_file` and the cache path (`elf/dl-cache.c`)
   become run-time paths, computed after `_dl_rtld_map.l_name` is set and
   before first use.
4. `@TERMUX_PREFIX@` stays as the build-time default/fallback; the live
   prefix overrides it.

### Caveats

- **Sites that cannot be dynamic.** Where a `_PATH_*` macro is used in
  `sizeof`, a static initializer, or a compile-time array, the site must be
  rewritten individually -- a blanket text replacement will not compile.
- **Ordering.** A few loader paths are needed very early; the derivation must
  complete before the first use (safe at the 1706 mark).
- **Writer/reader must agree.** `ldconfig` must derive the same prefix and
  write `<prefix>/usr/etc/ld.so.cache`; the loader reads exactly there.
- **Fallback.** When self-derivation yields nothing (`dladdr` fails, or the
  loader's `l_name` is empty because it was run directly as a command), fall
  back to the interpreter's path via `dl_iterate_phdr`, then to the compiled
  default, rather than failing.

## The package set

- **Ours (shipped prebuilt):** `libc6` (loader + shared libs) and `libc-bin`
  (`ldconfig`, `ldd`, ...). Built by `bootstrap/dn-package-glibc.sh`
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

- `core/install/dn-translate-deb.sh` (`LD`): every translated glibc ELF's
  `PT_INTERP` becomes the fused loader, so the kernel loads it. The existing
  match (`*/ld-linux-aarch64.so.1`) is unchanged -- only the target.
- `core/install/dn-fix-gcc-specs.sh` (`LDDN`): gcc's `*link` spec writes the
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
[`tracer.md`](tracer/tracer.md), [`syscall-boundary.md`](../reference/syscall-boundary.md)). The
fused loader removes `ld-dn`, not the tracer.
