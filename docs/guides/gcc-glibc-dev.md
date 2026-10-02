# Guide: developing with gcc/glibc inside the prefix

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's summary
> and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing — a new spec topic, a new one-off
> investigation, or a new guide — not just a long addition to what a doc
> already covers.

How to actually compile, link, and run C programs *inside* the prefix —
not installing pre-built Debian packages, but using `apt install gcc` to
build your own code. Covers plain `gcc`, `make`, and shared libraries.
Status: all verified working end to end on-device
(`dn-fix-gcc-specs.sh`'s `PT_INTERP` fix, `TODO.md`/
`docs/log/findings/gcc-hello-pt-interp-gap.md`, 2026-10-01), one real
gotcha around `LD_LIBRARY_PATH` documented below
with two workarounds.

## Contents

- [Setup](#setup)
- [Compile, link, run](#compile-link-run)
- [`make` works](#make-works)
- [Shared libraries: `LD_LIBRARY_PATH` is ignored](#shared-libraries-ldlibrarypath-is-ignored)
- [Status](#status)

## Related docs

- [`../log/findings/gcc-hello-pt-interp-gap.md`](../log/findings/gcc-hello-pt-interp-gap.md)
  and [`../log/findings/patchelf-et-exec-runpath.md`](../log/findings/patchelf-et-exec-runpath.md)
  — the engineering trail for the `PT_INTERP`/`gcc` fixes this guide
  relies on.
- [`python-venv.md`](python-venv.md) — the other guide in this directory,
  for a different language (Python) hitting a different wall (no
  `manylinux` wheel) with a different mechanism (a separate venv, not
  the toolchain itself).

## Setup

```sh
dn-shell -c "apt install -y gcc libc6-dev make"
```

`gcc`, `cpp`, `binutils` and their whole dependency chain, plus
`libc6-dev` (Debian's real package, unmodified — no custom packaging
needed), install cleanly. `make` is a separate package, not pulled in
by `gcc`.

## Compile, link, run

Ordinary `gcc` usage, no special flags needed:

```sh
dn-shell -c "gcc -c hello.c -o hello.o"       # compile only
dn-shell -c "gcc -o hello hello.c && ./hello"  # compile, link, run
```

This works because of a fix already applied to this device's prefix
(`scripts/install/dn-fix-gcc-specs.sh`, wired into `dn-hook-post.sh`):
plain `gcc` on Debian hardcodes `-dynamic-linker
/lib/ld-linux-aarch64.so.1` into its link spec (baked into GCC's own
build, `aarch64-linux.h`'s `GLIBC_DYNAMIC_LINKER` macro) — a path that
does not exist on Android and that the kernel resolves itself at
`execve()` time, before the path-redirect shim or anything else in
userspace ever runs. The fix is a `specs` file dropped next to each
installed `gcc` version's `libgcc.a` (GCC's own site-customization
hook, no `gcc`/`binutils` patch or rebuild) that swaps just that one
string for `ld-dn`'s real, resolvable path. Confirm it's in place:

```sh
dn-shell -c "readelf -l hello | grep -A1 'program interpreter'"
# [Requesting program interpreter: .../usr/lib/deb-native/ld-dn]
```

If that line instead shows the literal `/lib/ld-linux-aarch64.so.1`,
the specs file is missing or stale — rerun
`scripts/install/dn-fix-gcc-specs.sh` (it's idempotent, a no-op if
`gcc` or `ld-dn` isn't present yet).

## `make` works

Previously untested (`TODO.md` had it flagged as an open unknown); now
confirmed:

```sh
dn-shell -c "apt install -y make"
```

An ordinary `Makefile` using the implicit `$(CC)` (which resolves to
`cc`, itself `gcc`) builds and runs with no special handling:

```make
hello: hello.c
	$(CC) -o hello hello.c
```

## Shared libraries: `LD_LIBRARY_PATH` is ignored

Building a `.so` works normally:

```sh
dn-shell -c "gcc -shared -fPIC -o libadd.so lib.c"
dn-shell -c "gcc -o main main.c -L. -ladd"
```

But running the result with a caller-set `LD_LIBRARY_PATH` pointing at
the `.so`'s own directory **does not work**, even though it's the
normal glibc way to do this:

```sh
dn-shell -c "LD_LIBRARY_PATH=. ./main"
# error while loading shared libraries: libadd.so: cannot open shared object file
```

**Why**: `native/ld-dn.c` — the interpreter every translated/adopted
program's `PT_INTERP` points at, including a plain `gcc`-linked binary
via the specs fix above — strips *any* inherited `LD_PRELOAD`,
`DN_INSTDIR`, `LD_LIBRARY_PATH`, and `COMPILER_PATH` from the process's
environment before `exec`-ing the real program, and replaces
`LD_LIBRARY_PATH` unconditionally with exactly two fixed directories:
`$DN/usr/lib/aarch64-linux-gnu` and `$DN/usr/lib` (the comment in
`ld-dn.c` explains why: one `LD_LIBRARY_PATH` set once here covers the
whole transitive load graph, replacing a per-`.deb` `RUNPATH` patch
that risked corrupting a tightly-packed `ET_EXEC` binary's program
headers — the same class of bug
`docs/log/findings/patchelf-et-exec-runpath.md` describes). A user-set
`LD_LIBRARY_PATH` is not
merged with this; it is discarded outright, every time, for every
program that goes through `ld-dn`.

**Three real workarounds** (confirmed, pick whichever fits):

1. **`DN_EXTRA_LIB_PATH`** (added 2026-10-02 specifically to close this
   gap — `native/ld-dn.c`, same escape-hatch convention as `DN_ID`,
   `docs/spec/design.md`): a colon-separated list of extra directories,
   appended to the two fixed ones, read fresh on every launch — no
   rebuild needed to use it for a new project:
   ```sh
   dn-shell -c "DN_EXTRA_LIB_PATH=/path/to/mylibs ./main"   # works
   ```
   Best for day-to-day development — point it at a project's own
   build directory and it's visible to every binary launched with it
   set, no install step, no relinking.

2. **Install the `.so` where `ld-dn` already looks, unconditionally**:
   ```sh
   dn-shell -c "cp libadd.so /usr/lib/ && ./main"   # works
   ```
   (`/usr/lib` here is the prefix's own, via `dn-shell`'s path
   redirection — not Termux's or Android's.) Fine for a library meant
   to be shared system-wide inside the prefix; not great for a
   project's own build artifacts, and needs no env var at the call
   site (useful for something invoked by a script you don't control).

3. **Rpath the binary at link time**, if you want it to work with no
   environment setup at all, from anywhere:
   ```sh
   dn-shell -c "gcc -o main main.c -L. -ladd -Wl,-rpath,'\$ORIGIN'"
   ```
   `\$ORIGIN` (escaped so the shell doesn't expand it — `ld` expands
   it itself, to the binary's own directory) makes the binary find a
   `.so` sitting next to it, with no reliance on `ld-dn`'s fixed
   search path or any env var.

`/usr/local/lib` — the other conventional place a developer might
expect to drop a library — does **not** work on its own; it's not one
of `ld-dn`'s two fixed directories (though `DN_EXTRA_LIB_PATH=/usr/local/lib`
would cover it).

## Status

Working end to end: `gcc`/`make`/shared-library builds all compile,
link, and run correctly inside the prefix. The `LD_LIBRARY_PATH`
gotcha (silently discarded, by design) now has a proper fix,
`DN_EXTRA_LIB_PATH`, in addition to the two existing workarounds —
no rebuild needed for a new project to use it, only the one-time
`ld-dn` rebuild that added the mechanism itself. `g++`/C++ untested
(not installed on this device yet); `rustc`/`ghc` remain unresearched
per `TODO.md`.
