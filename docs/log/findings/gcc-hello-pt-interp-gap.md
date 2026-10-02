# Findings: `gcc -o hello hello.c` -- the shim's `/lib` gap, and a separate `PT_INTERP` wall (2026-10-01)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing — a new spec
> topic, a new one-off investigation, or a new guide — not just a
> long addition to what a doc already covers.

**Impact: Open gap.** The shim's `/lib`/`/bin`/`/sbin` gap was found
and fixed in the same entry; the separate, deeper `PT_INTERP`
kernel-level wall it uncovered was flagged, not fixed, as of this
writing — see the note below on where that stood later.

With `libc6-dev` installed, `gcc -c hello.c` succeeded (the actual
blocker from [`apt-install-gcc-end-to-end.md`](apt-install-gcc-end-to-end.md)), but `gcc -o hello hello.c` (compile +
link) failed: `ld: cannot find /lib/aarch64-linux-gnu/libc.so.6`. `gcc`'s
linker invocation hardcodes `-dynamic-linker /lib/ld-linux-aarch64.so.1`
and searches `/lib/aarch64-linux-gnu` directly (confirmed with `gcc -v`),
regardless of how `libc6-dev`'s own files spell paths internally. This is
exactly the "`/bin`, `/sbin`, `/lib`" gap `shim-coverage.md` had flagged
as an open question since 2026-09-30 but left unmeasured -- now measured,
by the compiler toolchain instead of by corpus inspection first.

## Contents

- [The shim fix: /lib, /bin, /sbin](#the-shim-fix)
- [Separate wall: PT_INTERP at the kernel level](#separate-not-yet-fixed-running-the-freshly-linked-hello-fails-at-the-kernel-level)

## Related docs

- [`apt-install-gcc-end-to-end.md`](apt-install-gcc-end-to-end.md) —
  the entry this one directly follows up on.
- [`patchelf-et-exec-runpath.md`](patchelf-et-exec-runpath.md) — the
  earlier fix that moved `RUNPATH` into `ld-dn`'s per-launch
  environment, the same approach this entry's `PT_INTERP` wall cannot
  use (the kernel reads `PT_INTERP` before `ld-dn` ever runs).
- `docs/spec/design.md`'s `DN_EXTRA_LIB_PATH` escape hatch, and
  `docs/guides/gcc-glibc-dev.md` — later work building directly on top
  of this entry's territory (compiling and linking inside the prefix).

## The shim fix

Considered two fixes: a "full view" (redirect every top-level path
through the shim, matching a real chroot more closely) vs. extending the
existing targeted dispatch with just the three missing merged-usr
aliases. Chose the latter (full detail: `docs/spec/shim-coverage.md`'s
now-resolved "the five-prefix view may be too narrow" section) --
`native/path-redirect.c`'s `rewrite()` now also dispatches `/lib`, `/bin`
(second byte `'l'`/`'b'`, default `prelen = 4`) and `/sbin` (`'s'`,
`prelen = 5`); none collide with the existing five. These are Debian's
own merged-usr symlinks into `/usr/{lib,bin,sbin}`, which the prefix's
`base-files` already sets up identically inside `$DN`, so this completes
the existing `/usr` coverage rather than adding new scope. Verified: `ld`
now finds `libc.so.6` and links `hello` successfully, no errors.

## Separate, not yet fixed: running the freshly-linked `hello` fails at the kernel level

`cannot execute: required file not found` --
`PT_INTERP` on a plain `gcc`-produced binary is the literal
`/lib/ld-linux-aarch64.so.1` string `ld` wrote into it, and the kernel
resolves `PT_INTERP` itself at `execve()` time, before any userspace
code (the shim included) runs -- so the `/lib` redirect above, which
works at the libc-call layer, cannot reach this. This is the same
problem `native/ld-dn.c` exists to solve for installed Debian packages
(their `PT_INTERP` gets pointed at `ld-dn.c`'s real, resolvable path as
part of the install pipeline), but a binary freshly built with `gcc`
inside the prefix was never put through that step -- it's a new,
distinct gap (compiling *inside* the prefix, not just installing
pre-built `.deb`s into it), not yet designed or fixed as of this entry.
Flagged for the user rather than solved inline: fixing it means deciding
how (a linker wrapper that passes `-dynamic-linker <real ld-dn path>`, a
post-link `patchelf` step, or something else), which is an architecture
call like the `/lib` one above, not a one-line follow-on.
