# Manual overlay: sudo-less's "view" purpose, without a kernel view

Status: **prototyped and verified working**, against a real gap (not a
toy). `native/path-redirect.c` + `scripts/build-path-redirect.sh`.

## Overlay/view: confirmed dead on this device, with data

`prior-art.md` flagged the mount-namespace + overlayfs "view" as "assumed
blocked, not yet verified" and left it as an open question. Now verified,
with the actual syscall error, not a guess:

```
$ unshare -Ur echo test
unshare: unshare failed: Invalid argument
$ strace -f unshare -Ur echo test
unshare(CLONE_NEWUSER) = -1 EINVAL (Invalid argument)
```

`EINVAL` on `unshare(CLONE_NEWUSER)`, not `EPERM` — this isn't a policy
denial (SELinux blocking an otherwise-supported syscall), it's the kernel
itself not supporting unprivileged user namespaces at all
(`CONFIG_USER_NS` not built in, or built in but with a restriction that
surfaces this way). Plain mount namespace alone:

```
$ unshare -m echo test
unshare: unshare failed: Operation not permitted
```

`EPERM` here — expected, `CLONE_NEWNS` needs `CAP_SYS_ADMIN` or a paired
user namespace, neither available.

**FUSE, the other "userspace mount" option, is also closed:**
`/dev/fuse: Permission denied`, no `fusermount`/`fusermount3` installed.

Conclusion: there is no mount-based way, privileged or not, to make a
directory in this prefix show up at a real absolute path on this device.
Whether this holds on every Android device isn't claimed — this is one
device's kernel, confirmed by direct syscall trace, not a general Android
fact. But for this project's actual target environment, it's a hard wall,
not a "probably."

## What the overlay was for, mapped against what Termux already has

| sudo-less's view target | why a package needs it | Termux's actual path | overlay-based fix | feasible here? |
|---|---|---|---|---|
| `/usr`, `/etc`, `/opt` made to show the prefix's files | absolute paths compiled into the binary | no real `/usr`/`/etc` tree exists to overlay onto at all on stock Android (not just "root-owned," likely *absent* or unwritable regardless) | mount-namespace overlay | **no** — confirmed above |
| dynamic linker / library search path | binary's `DT_NEEDED` libraries | `$PREFIX/glibc/lib`, already the glibc `ld.so`'s own default search path | none needed | **already solved**, no overlay ever required (`design-native-deps.md`) |
| interpreter's own module search path (Python/Perl/...) | `#!/usr/bin/python3`-style scripts | env vars (`PYTHONPATH`, ...) | none needed | **already solved** via env vars (`design-static-wrappers.md`) |
| ELF interpreter (`.interp`) pointing at `/lib/ld-linux-...`| dynamic linker itself | `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` | ELF patch | **already solved**, `grun --configure` |
| a binary's own hardcoded absolute data/config path (`/usr/share/figlet`, `/etc/foo.conf`) read directly in C code, no env var, no CLI flag | genuinely needs *something* to sit at that absolute path | nothing — this is the one real gap | **this doc's approach**: libc call interposition (`LD_PRELOAD`), not a mount | **yes, verified working** |

Only the last row was ever actually unsolved. Everything else in the
table already has a working, non-overlay answer elsewhere in this repo's
docs. The "manual overlay" this doc adds is specifically for that one
remaining row.

## The mechanism: libc call interposition, not a filesystem view

`native/path-redirect.c` is an `LD_PRELOAD` shared library: it overrides
`open`, `openat`, `fopen`, `stat`, `fstatat`, and the older `__fxstatat`,
rewriting any path starting with a configured prefix
(`DN_REDIRECT_FROM`) to a different prefix (`DN_REDIRECT_TO`) before
calling the real libc function via `dlsym(RTLD_NEXT, ...)`. This is
userspace symbol interposition — the dynamic linker resolves the
program's calls to *this* library's functions instead of glibc's, because
`LD_PRELOAD` puts it first in the search order. No kernel feature beyond
ordinary dynamic linking is involved.

This is effectively "the view's job, done per-syscall instead of per
mount" — a "manual overlay" in the sense the request that prompted this
doc used the term: since the overlay itself is unbuildable, redirect the
specific calls that would have needed it, one prefix mapping at a time,
per-process (via env vars set alongside `LD_PRELOAD` when running the
wrapped binary — ties directly into `design-static-wrappers.md`'s wrapper
scripts: the wrapper sets `DN_REDIRECT_FROM`/`DN_REDIRECT_TO`/
`LD_PRELOAD` before `exec`ing the real binary).

## Verified against a real package, not a toy example

`figlet` (`figlet_2.2.5-3+b2_arm64.deb`, unmodified, from `deb.debian.org`)
— chosen because sudo-less's own `docs/view.md` cites this exact package
as a canonical case needing the view (`figlet (/usr/share/figlet)`).
Debian names the actual binary `figlet-figlet` (fronted by
`update-alternatives`, per sudo-less's own note about that pattern).

```
$ ld-linux-aarch64.so.1 figlet-figlet hi
figlet-figlet: standard: Unable to open font file
```

`strace` confirms exactly what: `fstatat(AT_FDCWD, "/usr/share/figlet/standard.flf", ...)
= -1 ENOENT` — a hardcoded absolute path, no environment variable reaches
it (checked: no `FIGLET_FONTDIR` or similar in this binary's behavior).

With the shim:

```
$ DN_REDIRECT_FROM=/usr/share/figlet DN_REDIRECT_TO=<font root> \
  LD_PRELOAD=native/path-redirect.so \
  ld-linux-aarch64.so.1 figlet-figlet "termux deb bridge"
 _                                       _      _
| |_ ___ _ __ _ __ ___  _   ___  __   __| | ___| |__
...
```

Full ASCII-banner output, correct. `DN_REDIRECT_DEBUG=1` prints each
rewrite for verification (`/usr/share/figlet/standard.flf -> .../standard.flf`).

## Toolchain gotchas hit building this (recorded so they aren't re-discovered)

Termux ships `*-glibc` packages' **runtime** libraries
(`termux-pacman/glibc-packages`) but not a full glibc **cross-toolchain**
for building new ones on-device. Compiling `native/path-redirect.c`
against Termux's own glibc, using Termux's own Bionic-hosted `clang`,
needed:

- `--target=aarch64-linux-gnu` (glibc target, not `aarch64-*-android`).
- **Not** `--sysroot=$GLIBC`: Termux's `$GLIBC/lib/libc.so` is a linker
  script (`GROUP ( /data/data/.../glibc/lib/libc.so.6 ... )`) with
  already-fully-resolved absolute paths baked in (Termux packages are
  built for one fixed install location, never relocated). Passing a real
  `--sysroot` makes `lld` re-root every absolute path in that script
  *again*, doubling it into a path that doesn't exist
  (`.../glibc` + `/data/data/.../glibc/lib/libc.so.6`) — confirmed via the
  exact `ld.lld` error message before finding the fix. `--sysroot=/`
  disables that re-rooting so the script's already-absolute paths resolve
  directly.
- `-nostartfiles -nodefaultlibs`: no `crtbeginS.o`/`crtendS.o`/`libgcc.a`
  shipped for this target on-device (only the *runtime* `libgcc_s.so` is
  present) — fine for a plain C shared library with no C++ exceptions;
  would block building a full executable's `_start`/`crt1.o` chain
  (confirmed separately — building a standalone executable this way still
  fails on missing `Scrt1.o`/`crti.o`/`crtn.o`, unresolved, not needed
  for this shim since `LD_PRELOAD` targets are always shared libraries).

## Scope limit found later: doesn't reach maintainer scripts

`docs/findings-survey-apt-2026-09-25.md` found this the hard way: this
shim only helps **glibc dynamically-linked binaries**. A real package's
`postinst` doing `. /usr/share/debconf/confmodule` or
`ln -s ... /usr/lib/ssl` hits the exact same "hardcoded absolute path,
nothing there" problem this doc solves for binaries — sudo-less's own
`design.md` says outright **"no shims needed so far (the view made
`py3compile`'s unnecessary)"**: this class of problem is exactly what
*the view*, not a "shim" in sudo-less's sense (a narrower thing — a fake
stand-in for a root-only helper program), solved for them. Since the view
is dead here, this project needed its own answer.

### Dead end, fully explored: a Bionic `LD_PRELOAD` shim

First attempt: a Bionic build of `path-redirect.c`'s idea
(`native/path-redirect-bionic.c`, plain `clang`, no cross-compile needed —
Bionic is native here), generalized to a wholesale `/usr`, `/etc`, `/var`,
`/opt` → `$INSTDIR` mapping (the same four directories the view
overlaid), `LD_PRELOAD`ed into dpkg's environment before it forks
maintainer scripts.

- **Confirmed dpkg preserves `LD_PRELOAD` into scripts**: a throwaway test
  `.deb`'s `postinst` dumped its own environment and showed
  `LD_PRELOAD=<the .so>` verbatim — dpkg does not clear it.
- **The shim itself works correctly**: verified against a freshly-compiled
  test binary calling `open()` — redirected, with debug output to prove
  it.
- **But the real target turned out to be a different binary than
  assumed.** The maintainer script's `#!/bin/sh` shebang is resolved by
  the *kernel*, against the real filesystem root — which on Android is
  `/system/bin/sh` (a root-owned Android **toybox** binary), confirmed
  directly (`readlink -f /proc/$$/exe` from inside a running maintainer
  script printed `/system/bin/sh`), **not** Termux's own `/bin/sh`
  (`dash`) as first assumed. Termux's `dash` was tested too and also
  resisted interception (linked `BIND_NOW`/`FLAGS_1 NOW` — Bionic's linker
  doesn't honor `LD_PRELOAD`'s override on `BIND_NOW` binaries the way
  glibc does), but it turned out to be the wrong binary to even chase:
  `/system/bin/sh` is root-owned, on a read-only system partition — not
  patchable, not rebuildable, not ours to touch at all.

Abandoned as a dead end: even if `BIND_NOW` weren't a problem, this
approach needs write access to (or the ability to rebuild) whatever
`/bin/sh` really is, and on Android that binary belongs to the OS, not to
this project's own writable prefix.

### What actually worked: rewrite the script text, not the runtime

`scripts/patch-maintainer-scripts.sh`, run between dpkg's `--unpack` and
`--configure` (already two separate steps in this project's pipeline —
see `design-install-path.md`): plain `sed`, rewriting any `/etc/`,
`/usr/`, `/var/`, `/opt/` path component in a package's `postinst`/
`preinst`/`postrm`/`prerm` to the same path under `$INSTDIR`, before dpkg
ever executes the script. No interception, no linker, no `LD_PRELOAD`, no
dependency on which binary `/bin/sh` happens to be.

This works because maintainer scripts are **shell scripts**: their paths
are almost always literal strings dpkg unpacks to disk as plain text
*before* running them, unlike a compiled binary's paths (which can be
runtime-computed, string-concatenated, or simply invisible to a text
tool). Verified end to end: a test `postinst` doing `. /etc/foo.conf`
failed before this ran and printed the sourced file's real content
correctly after.

Known limits (not yet hit in practice, worth stating): a script that
builds a path at runtime (`dir=/usr; . "$dir/share/foo"`) won't be
caught by a literal-string `sed`; a value carried through a variable
already set before the rewrite runs is invisible to it. Falls back to
the same "genuinely can't reach this without a much bigger mechanism"
bucket as a statically-linked binary's hardcoded paths, for the
`LD_PRELOAD` shim above.

## Open work

- [ ] Generalize past one hardcoded `DN_REDIRECT_FROM`/`_TO` pair to a
      real mapping table (multiple prefixes per process) — needed once
      this is wired into the wrapper-generation pipeline
      (`design-hooks.md`) for packages with more than one hardcoded path.
- [ ] Intercept more of the relevant libc surface as real packages expose
      gaps: `access`, `readlink`, `opendir`, `execve` (a binary that
      `exec`s another absolute path, sudo-less's own `figlet` →
      `figlet-figlet` alternative-link case), `realpath`.
- [ ] Decide how the wrapper script generated per binary
      (`design-static-wrappers.md`) computes each binary's
      `DN_REDIRECT_FROM`/`_TO` pairs — likely from `prefix-wrap`-style
      detection (which absolute paths under `/usr`, `/etc`, `/opt` does
      this package's own file list touch) rather than hand-set env vars.
- [ ] Statically-linked binaries (no dynamic libc calls to intercept) are
      not reachable by this mechanism at all — same as `LD_PRELOAD`'s
      general limitation, flagged in `design-static-wrappers.md`, now
      confirmed as the actual remaining unreachable case rather than a
      theoretical one.
- [ ] Confirm whether the `EINVAL` on `CLONE_NEWUSER` is universal across
      Android kernels/devices or specific to this one (5.10, this vendor's
      kernel build) — doesn't change this project's direction either way
      (the manual-overlay approach needs no namespace and works
      regardless), but affects how confidently `docs/prior-art.md`'s
      "blocked on Android" claim can be generalized to other devices.
