# Findings: `gcc -o hello hello.c` -- the shim's `/lib` gap, and a separate `PT_INTERP` wall (2026-10-01)

<!-- template: templates/docs.template.md -->

**Impact: Resolved 2026-10-01.** The shim's `/lib`/`/bin`/`/sbin` gap and
the separate, deeper `PT_INTERP` kernel-level wall it uncovered were both
found and fixed in this entry's follow-up work — see "Separate, fixed
2026-10-01" below.

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
- [Separate wall: PT_INTERP at the kernel level](#separate-fixed-2026-10-01-running-the-freshly-linked-hello-failed-at-the-kernel-level)

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
aliases. Chose the latter (full detail: `docs/spec/shim/shim-coverage.md`'s
now-resolved "the five-prefix view may be too narrow" section) --
`core/native/path-redirect.c`'s `rewrite()` now also dispatches `/lib`, `/bin`
(second byte `'l'`/`'b'`, default `prelen = 4`) and `/sbin` (`'s'`,
`prelen = 5`); none collide with the existing five. These are Debian's
own merged-usr symlinks into `/usr/{lib,bin,sbin}`, which the prefix's
`base-files` already sets up identically inside `$DN`, so this completes
the existing `/usr` coverage rather than adding new scope. Verified: `ld`
now finds `libc.so.6` and links `hello` successfully, no errors.

## Separate, fixed 2026-10-01: running the freshly-linked `hello` failed at the kernel level

`cannot execute: required file not found` --
`PT_INTERP` on a plain `gcc`-produced binary is the literal
`/lib/ld-linux-aarch64.so.1` string `ld` wrote into it, and the kernel
resolves `PT_INTERP` itself at `execve()` time, before any userspace
code (the shim included) runs -- so the `/lib` redirect above, which
works at the libc-call layer, cannot reach this. This is the same
problem `core/native/ld-dn.c` exists to solve for installed Debian packages
(their `PT_INTERP` gets pointed at `ld-dn.c`'s real, resolvable path as
part of the install pipeline), but a binary freshly built with `gcc`
inside the prefix was never put through that step.

Confirmed where the literal string comes from: `gcc -dumpspecs`'s `*link`
spec embeds `-dynamic-linker ... /lib/ld-linux-aarch64%{mbig-endian:_be}
%{mabi=ilp32:_ilp32}.so.1` directly (Debian's gcc-14 inlines it inline,
not via a separate `%(dynamic_linker)` subspec) -- this string comes from
GCC's own source (`gcc/config/aarch64/aarch64-linux.h`'s
`GLIBC_DYNAMIC_LINKER` macro), baked in at GCC's own build time.

No gcc/binutils patch or rebuild needed, though: GCC auto-reads an
optional `specs` file from the same directory as its `libgcc.a` (`gcc
-print-libgcc-file-name`'s dirname) if one is present, letting a site
override any built-in default without recompiling gcc -- the exact
mechanism musl-based and Android NDK toolchains use for the same kind of
problem. Verified by hand first: `gcc -dumpspecs > specs`, `sed` the one
dynamic-linker literal to `ld-dn`'s real path
(`$DN/usr/lib/deb-native/ld-dn`), copy to
`$DN/usr/lib/gcc/aarch64-linux-gnu/14/specs` -- `gcc -v` then shows
`Reading specs from .../specs` and the resulting `-dynamic-linker` is
`ld-dn`'s path; `readelf -l hello` confirms the `PT_INTERP` segment
changed accordingly, and `./hello` runs and prints its output.

Promoted to a real fix: `core/install/dn-fix-gcc-specs.sh` (new),
wired into `dn-hook-post.sh` (apt's `DPkg::Post-Invoke`, so it reruns
after every `apt install`/`upgrade`, not just once at bootstrap). For
every `$DN/usr/lib/gcc/*/*` version directory with a matching gcc binary
installed, generates that version's own specs file the same way, with
only the dynamic-linker literal swapped; skipped once a version's specs
file already contains `ld-dn`'s path (idempotent), and a no-op entirely
if gcc or `ld-dn` isn't present yet (covers a fresh prefix with no
compiler installed, and bootstrap ordering). Verified end to end:
removed the manually-placed specs file, reran the script standalone
(regenerates it), reran it again (no-op, no error), then `gcc -o hello
hello.c && ./hello` through the real `dn-shell` -- prints its output,
exit 0.
