# glibc dynamic-linker mechanics (reference)

<!-- template: templates/docs.template.md -->

A catalog of how glibc's dynamic linker (`ld.so` / `ld-linux-*.so.N`)
decides what to load and when, grounded in the glibc 2.41 source tree
(`elf/`). It exists to design the **`dn-glibc`** fused loader: knowing
which surfaces already exist is what lets that patch stay small instead of
re-implementing environment and search-path logic. Reference doc, tracks
glibc 2.41 as fetched for the amd64 development tree.

## Contents

- [Invocation modes](#invocation-modes)
- [Library search order](#library-search-order)
- [File-based config](#file-based-config)
- [Environment variables](#environment-variables)
- [Command-line options](#command-line-options)
- [Preload sources](#preload-sources)
- [Auditing](#auditing)
- [Dynamic-section tags](#dynamic-section-tags)
- [Hardware capabilities](#hardware-capabilities)
- [Dynamic string tokens](#dynamic-string-tokens)
- [Relevant to dn-glibc](#relevant-to-dn-glibc)
- [How this slims the loader patch](#how-this-slims-the-loader-patch)

## Related docs

- [`ld-dn-runtime.md`](../log/ld-dn-runtime.md) -- the interpreter trampoline that
  `dn-glibc` would replace.
- [`ld-dn-config.md`](../log/ld-dn-config.md) -- the policy vocabulary the fused
  loader inherits (as `dn-glibc.conf`).
- [`elf-interp-patch.md`](elf-interp-patch.md) -- the static `PT_INTERP`
  patch that makes the kernel pick the loader at all.
- [`android-platform.md`](android-platform.md) -- Android's enforcement
  gates (seccomp/capability/SELinux), the reason the loader is a fork.

## Invocation modes

Two, and they differ in what is available to you:

- **As `PT_INTERP`** (the kernel `execve()`s the loader): **no
  command-line options**. The only inputs are the **environment** and
  **files** on disk (plus whatever is compiled in).
- **Directly** (`ld.so [OPTIONS] PROGRAM [ARGS]`): unlocks the command-line
  options below (`elf/dl-usage.c:185-203`, parsed in `elf/rtld.c:1375-1411`).

This is the crux for `dn-glibc`: our loader always runs as `PT_INTERP`, so
CLI options are unusable and injection must come from env, files, or the
patch itself.

## Library search order

From `ld.so(8)`; most-specific to least, first hit wins:

1. `DT_RPATH` (if `DT_RUNPATH` is absent)
2. `LD_LIBRARY_PATH`
3. `DT_RUNPATH` (direct dependencies only; not transitive)
4. `/etc/ld.so.cache` (compiled by `ldconfig`)
5. default `/lib`, then `/usr/lib`

`glibc-hwcaps` subdirectories are layered into each step (see below).

## File-based config

| File | Read by | Effect |
|---|---|---|
| `/etc/ld.so.conf` (+ `ld.so.conf.d/*.conf`) | **`ldconfig`, not the loader** | compiled into `/etc/ld.so.cache` (`elf/ldconfig.c:51`) |
| `/etc/ld.so.cache` | the loader (`elf/dl-cache.c:1`, `elf/dl-load.c:2047`) | search dirs, candidate libraries, and tunables |
| `/etc/ld.so.preload` | the loader at startup (`elf/rtld.c:1825`) | flat list of libraries preloaded before the program |
| `/etc/suid-debug` | the loader | re-enables `LD_DEBUG` under secure-execution mode |

Two facts matter for a prefix: the loader reads the **cache**, not the raw
`ld.so.conf` (so a search-dir change needs an `ldconfig` run), and every
path here is the loader's build-time **sysconfdir** -- a fixed location, not
redirectable by env.

## Environment variables

Read in `process_envvars` (`elf/rtld.c:2708`; secure variant `:2486`;
default variant `:2557`). The important ones:

- **`LD_LIBRARY_PATH`** -- extra search dirs (ignored in secure mode).
- **`LD_PRELOAD`** -- libraries loaded first (ignored/stripped in secure mode).
- **`LD_AUDIT`** -- audit modules (separate namespace).
- **`LD_DEBUG`**, **`LD_BIND_NOW`**, **`LD_PROFILE`**, **`LD_SHOW_AUXV`**,
  **`LD_TRACE_LOADED_OBJECTS`**, **`LD_WARN`**, **`LD_ORIGIN_PATH`**,
  **`LD_HWCAP_MASK`**.
- **`GLIBC_TUNABLES`** -- `key:value` list, parsed by
  `elf/dl-tunables.c` (see "Tunables" below).

Secure-execution mode (`AT_SECURE` nonzero: setuid/setgid, capabilities, or
an LSM) voids or strips most of these.

## Command-line options

Only when the loader is invoked directly (`elf/dl-usage.c:185-203`;
`elf/rtld.c:1375-1411`): `--library-path`, `--preload`, `--audit`,
`--inhibit-cache`, `--inhibit-rpath`, `--list`, `--list-diagnostics`,
`--list-tunables`, `--argv0`, `--verify`, `--glibc-hwcaps-prepend`,
`--glibc-hwcaps-mask`, `--help`, `--version`.

## Preload sources

Three sources **accumulate** (all are applied, in this order):

1. `LD_PRELOAD` (environment)
2. `--preload` (command line)
3. `/etc/ld.so.preload` (file)

Handled by `handle_preload_list` (`elf/rtld.c:850`, called from `:1805`,
`:1814`, `:1825`), then loaded by `_dl_map_object_deps` (`:1928`).
Preloaded objects are placed first in the link map, so they can interpose
symbols -- which is precisely how deb-native's `path-redirect.so` shim
works today, via the `ld.so.preload` file.

## Auditing

`LD_AUDIT` / `DT_AUDIT` / `DT_DEPAUDIT` / `--audit`
(`elf/dl-audit.c`, `rtld-audit(7)`): a **separate linker namespace** with
callbacks at checkpoints (`la_objopen`, `la_symbind`, `la_preinit`, ...).
It can observe and redirect loading without participating in normal symbol
binding. A heavier, more general hook than preload; noted for completeness,
not proposed for `dn-glibc`.

## Dynamic-section tags

Tags the loader honors: `DT_NEEDED`, `DT_RPATH`/`DT_RUNPATH`, `DT_SONAME`,
`DT_AUDIT`/`DT_DEPAUDIT`, `DT_FLAGS` (`BIND_NOW`), `DT_INIT` /
`DT_INIT_ARRAY` / `DT_FINI` / `DT_FINI_ARRAY`, `DT_PREINIT_ARRAY`, symbol
versioning (`DT_VERNEED` / `DT_VERDEF` / `DT_VERSYM`), `DT_RELRO`,
`DT_TEXTREL`. These describe a single object; they are not global config.

## Hardware capabilities

`glibc-hwcaps/<level>` subdirectories (`elf/dl-hwcaps.c`), e.g.
`glibc-hwcaps/x86-64-v3` or `glibc-hwcaps/aarch64-v2`, consulted in the
search path; plus legacy per-feature hwcap dirs (32-bit only, mostly
retired). Selects the best variant of a library for the running CPU.

## Dynamic string tokens

`$ORIGIN`, `$LIB`, `$PLATFORM` (and `${...}` forms) expand inside
`RPATH`/`RUNPATH`, the `LD_*` lists, `--library-path`/`--preload`, and
`dlopen`/`dlmopen` arguments.

## Relevant to dn-glibc

Running as `PT_INTERP`, only three kinds of surface are usable:

- **Environment** -- `LD_PRELOAD`, `LD_LIBRARY_PATH`, `DN_*`.
- **Files** -- `ld.so.preload`, `ld.so.cache` (via `ldconfig`), and a
  deb-native config file.
- **Compiled-in** -- whatever the patch does before mapping dependencies.

The **file** surfaces are the interesting ones, because they are
environment-free and, being read only by glibc's loader, are **glibc-only by
construction** (Bionic/Termux programs never see them) -- the same
selectivity the project now gets from `ld.so.preload` / `ld.so.cache`.

## How this slims the loader patch

Because glibc already implements search-dir and preload handling as
**data** (`ld.so.cache`/`ld.so.preload`, plus `DN_*`/`LD_*`), the fused
loader does not need to re-implement any of it. The patch reduces to:

1. **Derive the prefix** from the program's `PT_INTERP` (the irreducible
   core -- see `docs/log/ld-dn-runtime.md`, step C2).
2. **Point the standard surfaces at the prefix**: resolve
   `<prefix>/etc/ld.so.preload` and the prefix's `ld.so.cache` instead of
   the hardcoded host paths. Then the shim is just one line in the prefix's
   `ld.so.preload`, and search dirs are managed by the prefix's own
   `ldconfig` (Debian's `libc6` postinst already runs it) -- no code.
3. **Layer `dn-glibc.conf`** on top, only for what glibc has no concept of:
   per-program overrides, `dn-env`/`dn-unset`, `dn-redirect`, `dn-no-shim`.

That keeps the code delta to "prefix + path redirection + a small config
reader", and lets glibc's own, well-tested mechanisms carry the rest --
which is the whole point of the fuse-into-loader approach being *more*
efficient rather than a rewrite.
