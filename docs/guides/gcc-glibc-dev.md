# Guide: developing with gcc/glibc inside the prefix

<!-- template: templates/docs.template.md -->

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
- [`../spec/dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md) — the prefix's
  own glibc loader and the `ld.so.cache` this guide relies on.
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
(`core/install/dn-fix-gcc-specs.sh`, wired into `dn-hook-post.sh`):
plain `gcc` on Debian hardcodes `-dynamic-linker
/lib/ld-linux-aarch64.so.1` into its link spec (baked into GCC's own
build, `aarch64-linux.h`'s `GLIBC_DYNAMIC_LINKER` macro) — a path that
does not exist on Android and that the kernel resolves itself at
`execve()` time, before the dn-shim shim or anything else in
userspace ever runs. The fix is a `specs` file dropped next to each
installed `gcc` version's `libgcc.a` (GCC's own site-customization
hook, no `gcc`/`binutils` patch or rebuild) that swaps just that one
string for the prefix's loader's real, resolvable path. Confirm it's in
place:

```sh
dn-shell -c "readelf -l hello | grep -A1 'program interpreter'"
# [Requesting program interpreter: .../usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1]
```

If that line instead shows the literal `/lib/ld-linux-aarch64.so.1`,
the specs file is missing or stale — rerun
`core/install/dn-fix-gcc-specs.sh` (it's idempotent, a no-op if
`gcc` or the prefix's loader isn't present yet).

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

The prefix's own glibc loader — the interpreter every translated/adopted
program's `PT_INTERP` points at, including a plain `gcc`-linked binary via
the specs fix above — resolves the prefix's libraries from
`$DN/usr/etc/ld.so.cache` (built by our `ldconfig`), so the whole
transitive load graph resolves inside the prefix without patching every
`.so`'s `RUNPATH` (which risked corrupting a tightly packed `ET_EXEC`
binary's program headers:
[`../log/findings/patchelf-et-exec-runpath.md`](../log/findings/patchelf-et-exec-runpath.md)).
A caller-set `LD_LIBRARY_PATH` is honoured as standard glibc does, after
the cache's directories. **That search is the whole convention** — no
`-rpath`, no copying the library into the prefix, no special launcher
flag.

It covers every case the same way: a `gcc -o` output, a child process
that inherits the variable, and a runtime `dlopen`/`ctypes.CDLL` by bare
name ([`python-venv.md`](python-venv.md)).

## Status

Working end to end: `gcc`/`make`/shared-library builds all compile,
link, and run correctly inside the prefix. Shared libraries use the
standard library search, which the prefix's `ld.so.cache` resolves first
and the loader honours a caller's `LD_LIBRARY_PATH` after — one convention
for every case. `g++`/C++ untested (not installed on this device yet);
`rustc`/`ghc` remain unresearched per `TODO.md`.
