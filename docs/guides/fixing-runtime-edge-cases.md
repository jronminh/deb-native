# Guide: fixing a runtime edge case in the prefix

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's summary
> and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing — a new spec topic, a new one-off
> investigation, or a new guide — not just a long addition to what a doc
> already covers.

When a program in the prefix misbehaves at runtime — a path lands outside
the prefix, a library is not found, a call slips past the shim — reach for
the **cheapest layer that can see the problem**. This guide is the decision
path: symptom → layer → fix → rebuild cost. It is generic (applies to the
`dn-glibc` prefix); the layers themselves are specified
in [`../spec/path-shim.md`](../spec/path-shim.md),
[`../spec/dl-mechanics.md`](../spec/dl-mechanics.md),
[`../spec/syscall-boundary.md`](../spec/syscall-boundary.md), and
[`../spec/dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md).

Status: the decision table and the runtime layers are as built; the exact
shim-build command for the `dn-glibc` prefix is still being settled
(`docs/spec/dn-glibc-prefix.md`, "One small loader patch").

## Contents

- [The decision table](#the-decision-table)
- [Diagnosing first](#diagnosing-first)
- [Layer 1: the shim](#layer-1-the-shim)
- [Layer 2: the loader's two files](#layer-2-the-loaders-two-files)
- [Layer 3: dynamic tags](#layer-3-dynamic-tags)
- [Layer 4: environment (launchers)](#layer-4-environment-launchers)
- [Layer 5: the tracer](#layer-5-the-tracer)
- [Layer 6: build time (last resort)](#layer-6-build-time-last-resort)

## Related docs

- [`../spec/dl-mechanics.md`](../spec/dl-mechanics.md) — the catalog of
  glibc mechanisms (env, files, CLI, dynamic tags, tunables, `LD_AUDIT`).
  Read it before assuming a new edge needs a rebuild.
- [`../spec/shim-coverage.md`](../spec/shim-coverage.md) — which libc entry
  points the shim already covers, and the measured corpus data.
- [`../spec/syscall-boundary.md`](../spec/syscall-boundary.md) — what the
  shim cannot see at all (the tracer's job).
- [`../spec/dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md) — the fused
  loader these layers sit under today.

## The decision table

Find the symptom, then take the layer. Cheapest fixes are at the top; a
glibc rebuild is the last resort.

| Symptom | Layer | Fix | Rebuild |
| --- | --- | --- | --- |
| A path is opened/stat'd/written outside the prefix | shim (`path-redirect.c`) | interpose the one libc entry point that sees it | shim `.so` only |
| `error while loading shared libraries: … not found` | loader search | add the dir to `<prefix>/usr/etc/ld.so.conf.d/`, run `ldconfig` | none |
| Every program needs a startup hook (e.g. a new interposer) | preload | add the `.so` to `<prefix>/etc/ld.so.preload` | that `.so` only |
| One program needs a private library directory | dynamic tags | `patchelf --set-rpath` in `dn-translate-deb.sh` | none |
| A static/Bionic/raw-syscall/libc-internal path slips through | tracer | route through `dn-run`/`dn-trace` | tracer rebuild |
| An absolute path is baked into glibc binaries or caches | build time | `set-dirs.patch` / configure | glibc rebuild |
| The loader's own behaviour must change | build time | `elf/rtld.c` hunk in `dn-glibc-android.patch` | glibc rebuild |

## Diagnosing first

Run the program through the prefix's own entry so the environment is clean
(`dn-shell`, or a `dn-run` launcher), never directly from a raw Termux
shell — a leaked host `LD_PRELOAD` is its own confusing failure:

```sh
dn-shell -c "./your-program args"          # clean env, prefix PATH
DN_REDIRECT_DEBUG=1 ./your-program         # shim dumps root + every rewrite
```

Then narrow the layer:

```sh
dn-shell -c "readelf -l ./prog | grep -A1 interpreter"   # PT_INTERP
dn-shell -c "readelf -d ./prog | grep -E 'NEEDED|RPATH|RUNPATH'"
dn-shell -c "ldd ./prog"                                  # resolvable deps
dn-shell -c "ldconfig -p | grep <lib>"                    # what the cache has
```

The shim's debug line tells you whether a path was even seen
(`[path-redirect] root=… /etc/x -> <prefix>/etc/x`). If it never appears
for a path the program clearly uses, the call is not going through libc —
go to Layer 5.

## Layer 1: the shim

`native/path-redirect.c` interposes the libc functions that take a path
(`open`/`stat`/`execve`/…) and rewrites `/usr`, `/etc`, `/var`, `/opt`,
`/root`, `/lib`, `/bin`, `/sbin` to `<prefix>/…`. It is loaded once per
program — via `<prefix>/etc/ld.so.preload` — and it derives the
prefix from its own load path, so nothing has to be injected with it.

If a path is being read but the debug line never shows it, the entry point
is missing from the shim's coverage. Add just that function to
`native/path-redirect.c` (same pattern as its neighbours), rebuild the shim,
and reinstall it at `<prefix>/usr/lib/deb-native/path-redirect.so`. The
`ld.so.preload` string does not change. `../spec/shim-coverage.md` is the
current coverage list and the technique for measuring it.

## Layer 2: the loader's two files

Two fixed files decide what every program preloads and where it looks. Both
are *runtime* artifacts — no rebuild of anything:

- **`<prefix>/etc/ld.so.preload`** — one `.so` path per line, loaded into
  every program. This is where an extra interposer or a startup hook goes.
  **Note:** in the fused prefix inherited `LD_PRELOAD` is ignored on
  purpose; this file is the supported channel.
- **`<prefix>/usr/etc/ld.so.conf`** (+ `ld.so.conf.d/*.conf`) — the search
  directories, compiled by `ldconfig` into
  **`<prefix>/usr/etc/ld.so.cache`**. You do not edit the cache; you edit
  the conf and run the prefix's own `ldconfig`:

```sh
dn-shell -c "echo '<prefix>/opt/mylib' > <prefix>/usr/etc/ld.so.conf.d/localtest.conf"
dn-shell -c "ldconfig"
```

The fused prefix's `ldconfig` is our own prefix-targeted build
(`scripts/bootstrap/dn-package-libc-bin.sh`), so it writes the prefix's
cache rather than the host's.

## Layer 3: dynamic tags

For a directory only one program needs, a `RUNPATH` is local and needs no
rebuild of anything:

```sh
dn-shell -c "patchelf --set-rpath '<prefix>/opt/mylib' ./prog"
```

Do this in `dn-translate-deb.sh` at install time when it applies to a
package, not by hand. Caution: `patchelf` can corrupt a tightly-packed
`ET_EXEC`'s program headers when it has to grow the table — that is why the
project sets a whole-process `LD_LIBRARY_PATH`/cache instead of rewriting
every `.so`'s `RUNPATH`
([`../log/findings/patchelf-et-exec-runpath.md`](../log/findings/patchelf-et-exec-runpath.md)).

## Layer 4: environment (launchers)

Some variables are consumed before any prefix code runs, or are needed by
name (`PATH`, `HOME`, `TMPDIR`, `PYTHONPATH`, …). Those belong in the
launchers (`scripts/runtime/make-launchers.sh`) or `dn-run`, not in a shim.
Anything you can express as an environment variable is a launcher change —
no rebuild of the prefix's libraries.

## Layer 5: the tracer

Static binaries, Bionic binaries, code that calls `syscall()` or emits
`svc #0` directly, and the libc-internal reads the shim cannot interpose all
bypass the shim. They are the tracer's job: `dn-run` classifies the target
and routes it through `dn-trace`, whose syscall-level rewrite maps the guest
`/usr /etc /var /opt` onto the prefix for the whole process tree. If a new
class of binary misbehaves, the fix is a `make-launchers.sh` classification
or a tracer change — never a shim one. See
[`../spec/syscall-boundary.md`](../spec/syscall-boundary.md) and
[`../spec/tracer.md`](../spec/tracer.md).

## Layer 6: build time (last resort)

Only two things force a glibc rebuild, and both are *build-time constants*,
not runtime behaviour:

- **A path baked into glibc.** `SYSCONFDIR` (the `ld.so.cache`/`ld.so.preload`
  location), the default search dir, gconv/locale dirs, and the guest `/etc`
  retarget are all compile-time. Changing them is a `set-dirs.patch` /
  configure change plus a rebuild (`third_party/glibc-android-patches/`).
- **The loader's semantics.** The inherited-`LD_PRELOAD` drop is an example
  (`dn-glibc-android.patch`'s `elf/rtld.c` hunk): it changes what the loader
  *does*, so it is a rebuild.

Before going here, re-read [`../spec/dl-mechanics.md`](../spec/dl-mechanics.md):
glibc exposes env vars, `ld.so.conf`, `ld.so.preload`, dynamic tags,
`$ORIGIN`, tunables, and `LD_AUDIT`, and a surprising number of "edge cases"
are one of those, not a patch.
