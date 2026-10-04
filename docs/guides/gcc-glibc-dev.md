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
`docs/log/findings/gcc-hello-pt-interp-gap.md`, 2026-10-01). Shared
libraries use the standard `LD_LIBRARY_PATH`, documented below.

## Contents

- [Setup](#setup)
- [Compile, link, run](#compile-link-run)
- [`make` works](#make-works)
- [Shared libraries: use `LD_LIBRARY_PATH`](#shared-libraries-use-ldlibrarypath)
- [Status](#status)

## Related docs

- [`../log/findings/gcc-hello-pt-interp-gap.md`](../log/findings/gcc-hello-pt-interp-gap.md)
  and [`../log/findings/patchelf-et-exec-runpath.md`](../log/findings/patchelf-et-exec-runpath.md)
  — the engineering trail for the `PT_INTERP`/`gcc` fixes this guide
  relies on.
- [`../spec/ld-dn-config.md`](../log/ld-dn-config.md) — the loader whose
  `LD_LIBRARY_PATH` policy this guide relies on.
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

## Shared libraries: use `LD_LIBRARY_PATH`

Building a `.so` works normally, and so does the standard glibc way of
pointing a program at it:

```sh
dn-shell -c "gcc -shared -fPIC -o libadd.so lib.c"
dn-shell -c "gcc -o main main.c -L. -ladd"
dn-shell -c "LD_LIBRARY_PATH=. ./main"     # works
```

`native/ld-dn.c` — the interpreter every translated/adopted program's
`PT_INTERP` points at, including a plain `gcc`-linked binary via the
specs fix above — sets `LD_LIBRARY_PATH` itself, so the whole transitive
load graph resolves inside the prefix (one variable set once, instead of
patching every `.so`'s `RUNPATH`, which risked corrupting a tightly
packed `ET_EXEC` binary's program headers:
[`../log/findings/patchelf-et-exec-runpath.md`](../log/findings/patchelf-et-exec-runpath.md)).
It puts the two fixed prefix directories
(`$DN/usr/lib/aarch64-linux-gnu`, `$DN/usr/lib`) first, so the prefix's
own libraries always win, then merges the caller's entries after them,
deduplicated. Before 0.5.3 a caller-set `LD_LIBRARY_PATH` was discarded
outright; since 0.5.3 it is honoured. **That one variable is the whole
convention** — no `-rpath`, no copying the library into the prefix, no
special launcher flag.

It covers every case the same way: a `gcc -o` output, a child process
that inherits the variable, and a runtime `dlopen`/`ctypes.CDLL` by bare
name ([`python-venv.md`](python-venv.md)).

## Status

Working end to end: `gcc`/`make`/shared-library builds all compile,
link, and run correctly inside the prefix. Shared libraries use the
standard `LD_LIBRARY_PATH`, which `ld-dn` sets (prefix directories
first) and merges the caller's entries into — one convention for every
case. `g++`/C++ untested (not installed on this device yet);
`rustc`/`ghc` remain unresearched per `TODO.md`.
