# Findings: fused loader -- the shim self-derives its prefix (2026-10-03)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing -- a new spec topic, a new one-off
> investigation, or a new guide -- not just a long addition to what a doc
> already covers.

**Impact: Repo change.** Toward the `dn-glibc` fused loader (fold `ld-dn`
into glibc's own loader; `docs/log/ld-dn-runtime.md`, "Alternative: fuse
into the loader"; `TODO.md`, "Runtime overhaul"): recon showed glibc can
already do the shim-injection and library-search jobs from files, so the
only piece missing was the prefix the shim needs. `native/path-redirect.c`
now derives that prefix from its own load path, with no injected env.
Proven on `fe2`.

## Contents

- [Context](#context)
- [What changed](#what-changed)
- [The test](#the-test)
- [Result](#result)
- [What it means](#what-it-means)
- [Not done yet](#not-done-yet)

## Related docs

- [`../../spec/ld-dn-runtime.md`](../../log/ld-dn-runtime.md) -- the
  trampoline, and the "Alternative: fuse into the loader" plan this serves.
- [`../../spec/dl-mechanics.md`](../../spec/dl-mechanics.md) -- the loader
  mechanisms (`ld.so.preload`, `ld.so.cache`) this leans on.
- [`../../spec/path-shim.md`](../../spec/path-shim.md) -- the shim itself.

## Context

The fused-loader plan replaces `ld-dn`'s job -- derive the prefix, inject
the shim, set the library path -- with glibc's own mechanisms. Recon on the
0.5.0 own-glibc tree (`~/dn-glibc-build/work`) showed those are already
prefix-pointed, because `set-dirs.patch` builds for the fixed prefix:

- `work/elf/rtld.c:1852`:
  `static const char preload_file[] = "/data/data/com.termux/files/home/.dn/etc/ld.so.preload";`
- `LD_SO_CACHE` = `SYSCONFDIR "/ld.so.cache"`, and `objdir/config.make` has
  `sysconfdir = ${prefix}/etc`.

So the shim can load from a prefix file and libraries resolve from a prefix
cache, both with no env. The one remaining dependency was `DN_INSTDIR`:
`path-redirect.c`'s constructor reads it (`:108`), and without it
`rewrite()` returns paths unchanged (`:155`). If nothing injects it, the
shim cannot know the prefix.

## What changed

`native/path-redirect.c`'s `dn_init()` now keeps `DN_INSTDIR` when set,
but falls back to `dladdr()` when it is not: the shim's own load path is
`<prefix>/usr/lib/deb-native/path-redirect.so` (wherever it is listed, in
`ld.so.preload` or `LD_PRELOAD`), so stripping that suffix yields the
prefix. A `DN_REDIRECT_DEBUG` line now prints the resolved root.

## The test

On `fe2`, with **no** `.dn` changes and no own-glibc needed:

- built the shim to `~/dn-glibc-test/usr/lib/deb-native/path-redirect.so`;
- created `~/dn-glibc-test/etc/marker` = `FUSED-OK`;
- preloaded that shim by absolute path with `DN_REDIRECT_DEBUG=1` and
  **`DN_INSTDIR` unset**, running a glibc `dash` through Termux's loader
  directly (`.../glibc/lib/ld-linux-aarch64.so.1 --library-path ...`), so
  the shim ran with no injector and a path it did not control the prefix of.

## Result

```
FUSED-OK
[path-redirect] root=/data/data/com.termux/files/home/dn-glibc-test
[path-redirect] /etc/marker -> /data/data/com.termux/files/home/dn-glibc-test/etc/marker
```

The shim derived the prefix from its own path and redirected `/etc/marker`
into it. Self-sufficient: no `DN_INSTDIR`, no `ld-dn`.

## What it means

The fused-loader core is now fully env-free and file-driven:

- **shim load** -> `<prefix>/etc/ld.so.preload`;
- **prefix for the shim** -> self-derived (`dladdr`);
- **library search** -> `<prefix>/etc/ld.so.cache` via `ldconfig`.

`ld-dn`'s whole job retires; the loader needs no code beyond being the
loader. Commits on branch `dn-glibc`: `8a42a79` (derivation), `915415f`
(debug line).

## Not done yet

The **full** fused test -- own-glibc loader + shim wired through
`<prefix>/etc/ld.so.preload` + `ldconfig` -- needs the shim **rebuilt
against own-glibc**: an earlier attempt loading the Termux-glibc-built shim
under own-glibc's loader failed with
`libc.so.6: version 'LIBC' not found` (Termux glibc's `LIBC` symbol version
vs Debian glibc's `GLIBC`). Since own-glibc is compiled for the fixed `.dn`
prefix, a separate test prefix also needs a rebuild with that prefix (or
`.dn`-shaped paths). Deferred to the next step.
