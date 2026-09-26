# Design

How deb-native works: we fake the Debian layout, not root — with our own
libc-interposition shim — plus native dependency reuse, the install path,
run-time wrappers, the apt/dpkg hooks, services, and prior art. (Merged from
the former `design-*.md`, `services-research.md` and `prior-art.md`.)

## Scope and design philosophy

The goal is deliberately narrow (the same shape as `sudo-less`): a Debian
package should **install** — reach `dpkg` status `ii` — and its program should
**run by name**, unprivileged. Not faithful Debian emulation, not isolation.

The design follows from that. Everything heavy underneath is *real*: real
aarch64, real glibc from Termux's side-install, Termux's real `apt`/`dpkg`,
real ELF loading. The only thing faked is the **layout** — that `/usr /etc /var
/opt` exist. Detail is therefore spent only at the **seams** that decide
"installed" and "runs":

- the prefix (where files actually land) and the path interposition;
- the maintainer-script exec path (`native/dn-launch.c`), because `preinst`/
  `postinst` run outside the process we control;
- dpkg's own state, which is real bookkeeping, not a bluff.

Everything in between can be ignored. And because this is an **adapter, not an
emulator**, coverage is a named boundary rather than a promise: paths that go
around libc — static binaries, raw `syscall()`, libc-internal `dlopen`, socket
`sun_path` — are out of scope until the syscall-level tracer sketched below and
in `TODO.md` / issue #1.

## Faking the Debian layout with our own shim (no kernel view)


Status: **prototyped and verified working**, against a real gap (not a
toy). `native/path-redirect.c` + `scripts/build-path-redirect.sh`.

### Overlay/view: confirmed dead on this device, with data

`design.md` flagged the mount-namespace + overlayfs "view" as "assumed
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

### What the overlay was for, mapped against what Termux already has

| sudo-less's view target | why a package needs it | Termux's actual path | overlay-based fix | feasible here? |
|---|---|---|---|---|
| `/usr`, `/etc`, `/opt` made to show the prefix's files | absolute paths compiled into the binary | no real `/usr`/`/etc` tree exists to overlay onto at all on stock Android (not just "root-owned," likely *absent* or unwritable regardless) | mount-namespace overlay | **no** — confirmed above |
| dynamic linker / library search path | binary's `DT_NEEDED` libraries | `$PREFIX/glibc/lib`, already the glibc `ld.so`'s own default search path | none needed | **already solved**, no overlay ever required (`design.md`) |
| interpreter's own module search path (Python/Perl/...) | `#!/usr/bin/python3`-style scripts | env vars (`PYTHONPATH`, ...) | none needed | **already solved** via env vars (`design.md`) |
| ELF interpreter (`.interp`) pointing at `/lib/ld-linux-...`| dynamic linker itself | `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` | ELF patch | **already solved**, `grun --configure` |
| a binary's own hardcoded absolute data/config path (`/usr/share/figlet`, `/etc/foo.conf`) read directly in C code, no env var, no CLI flag | genuinely needs *something* to sit at that absolute path | nothing — this is the one real gap | **this doc's approach**: libc call interposition (our own shim), not a mount | **yes, verified working** |

Only the last row was ever actually unsolved. Everything else in the
table already has a working, non-overlay answer elsewhere in this repo's
docs. The "manual overlay" this doc adds is specifically for that one
remaining row.

### The mechanism: libc call interposition, not a filesystem view

`native/path-redirect.c` is our own shim — a glibc shared object we build
and preload into the process. It overrides
`open`, `openat`, `fopen`, `stat`, `fstatat`, and the older `__fxstatat`,
rewriting any path starting with a configured prefix
(`DN_REDIRECT_FROM`) to a different prefix (`DN_REDIRECT_TO`) before
calling the real libc function via `dlsym(RTLD_NEXT, ...)`. This is
userspace symbol interposition — the dynamic linker resolves the
program's calls to *this* library's functions instead of glibc's, because
preloading our shim puts it first in the search order. No kernel feature beyond
ordinary dynamic linking is involved.

This is effectively "the view's job, done per-syscall instead of per
mount" — a "manual overlay" in the sense the request that prompted this
doc used the term: since the overlay itself is unbuildable, redirect the
specific calls that would have needed it, one prefix mapping at a time,
per-process (via env vars set alongside `LD_PRELOAD` when running the
wrapped binary — ties directly into `design.md`'s wrapper
scripts: the wrapper sets `DN_REDIRECT_FROM`/`DN_REDIRECT_TO`/
`LD_PRELOAD` before `exec`ing the real binary).

### Verified against a real package, not a toy example

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

### Toolchain gotchas hit building this (recorded so they aren't re-discovered)

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

### Scope limit found later: doesn't reach maintainer scripts

`docs/findings.md` found this the hard way: this
shim only helps **glibc dynamically-linked binaries**. A real package's
`postinst` doing `. /usr/share/debconf/confmodule` or
`ln -s ... /usr/lib/ssl` hits the exact same "hardcoded absolute path,
nothing there" problem this doc solves for binaries — sudo-less's own
`design.md` says outright **"no shims needed so far (the view made
`py3compile`'s unnecessary)"**: this class of problem is exactly what
*the view*, not a "shim" in sudo-less's sense (a narrower thing — a fake
stand-in for a root-only helper program), solved for them. Since the view
is dead here, this project needed its own answer.

#### Dead end, fully explored: a Bionic preload shim

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

#### What actually worked: rewrite the script text, not the runtime

`scripts/patch-maintainer-scripts.sh`, run between dpkg's `--unpack` and
`--configure` (already two separate steps in this project's pipeline —
see `design.md`): plain `sed`, rewriting any `/etc/`,
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
shim above.

### Open work

- [ ] Generalize past one hardcoded `DN_REDIRECT_FROM`/`_TO` pair to a
      real mapping table (multiple prefixes per process) — needed once
      this is wired into the wrapper-generation pipeline
      (`design.md`) for packages with more than one hardcoded path.
- [x] Intercept the relevant libc surface: `access`, `readlink`, `opendir`,
      `execve` (a binary that `exec`s another absolute path, sudo-less's own
      `figlet` → `figlet-figlet` alternative-link case) and `realpath` are
      covered, along with the rest of the path-taking entry points
      (`creat`, `freopen`, `chown`/`lchown`/`fchownat`, the xattr family,
      `mkfifo`/`mknod`, `statfs64`/`statvfs64`, `inotify_add_watch`, AF_UNIX
      `sendto`, `mkstemp`/`mkdtemp`, `posix_spawn`/`posix_spawnp`).
      `tests/shim-libc/run.sh` asserts each one on-device. What libc
      interposition cannot reach — static binaries, raw `syscall()`,
      libc-internal opens — is the syscall tracer's job.
- [ ] Decide how the wrapper script generated per binary
      (`design.md`) computes each binary's
      `DN_REDIRECT_FROM`/`_TO` pairs — likely from `prefix-wrap`-style
      detection (which absolute paths under `/usr`, `/etc`, `/opt` does
      this package's own file list touch) rather than hand-set env vars.
- [ ] Statically-linked binaries (no dynamic libc calls to intercept) are
      not reachable by this mechanism at all — same as `LD_PRELOAD`'s
      general limitation, flagged in `design.md`, now
      confirmed as the actual remaining unreachable case rather than a
      theoretical one.
- [ ] Confirm whether the `EINVAL` on `CLONE_NEWUSER` is universal across
      Android kernels/devices or specific to this one (5.10, this vendor's
      kernel build) — doesn't change this project's direction either way
      (the manual-overlay approach needs no namespace and works
      regardless), but affects how confidently `docs/design.md`'s
      "blocked on Android" claim can be generalized to other devices.
## Native dependency reuse (sudo-less's "native" idea, adapted)


Status: **prototyped and verified working**, `scripts/native-seed.sh`.
Builds on [`design.md`](design.md)'s two-layer-db
idea and corrects it with what `findings.md` found.

### The idea, restated for this project

sudo-less's original two-layer database seeds the prefix's dpkg status
from the *host's* `/var/lib/dpkg/status`, so already-present system
libraries count as satisfied and only leaf packages get installed into
`~/.local` — everything ends up inside that one directory either way (the
host's copies stay where they are; only the missing pieces get added
under the prefix).

This project does the same thing, but the "host" being pointed at isn't a
generic `/`: it's Termux's own curated glibc side-install
(`termux-pacman/glibc-packages`, `$PREFIX/glibc/...`). And unlike
sudo-less, this repo deliberately does **not** put everything under one
`.local`-style tree — a Debian dependency Termux already provides natively
is left exactly where Termux's own package manager put it
(`$PREFIX/glibc/lib/libz.so.1`, found by the glibc dynamic linker's own
default search path, not by any `LD_LIBRARY_PATH` trick). Only what
Termux's glibc side-install does *not* already have lands in the project's
own collection point (`$INSTDIR`, `~/.dn` by default) —
that's the one place matching sudo-less's `.local` role, but scoped to the
actual delta instead of everything.

### How it works: `scripts/native-seed.sh`

Before unpacking a `.deb`, seed `$ADMINDIR/status` with synthetic
`Status: install ok installed` stanzas for Debian package names already
covered by an installed Termux `*-glibc` package — no files copied, dpkg
just treats the dependency as met. dpkg's own "no files list" handling
(already a normal code path, used for essential packages with no tracked
file list) takes over gracefully; verified in practice, not just assumed:
dpkg prints a warning ("files list file... missing; assuming package has
no files currently installed") and proceeds normally.

### Verified end to end against a real package with a real dependency

`ciso` (`ciso_1.0.2-2+b1_arm64.deb`, from `deb.debian.org`) — a genuine,
tiny CLI tool (PSP ISO↔CSO converter) whose only dependencies are `libc6`
and `zlib1g`. Chosen specifically because it's a real-world case that
*needs* this native-reuse mechanism, not the simplest possible case
(`hello`, tested first, has no non-libc dependency at all).

```
$ scripts/prototype-install.sh ciso.deb
==> seeding natively-satisfied dependencies into .../var/lib/dpkg
seeded: libc6 <- glibc 2.44
seeded: zlib1g <- zlib-glibc 1.3.2
...
==> unpacking ciso into .../root
==> configuring ciso
Setting up ciso:arm64 (1.0.2-2+b1) ...
==> patching new ELF binaries with grun --configure
   patched: /usr/bin/ciso
```

Confirmed by inspection:

```
$ find .../root -iname '*libz*' -o -iname '*libc.so*'
# (nothing — zlib1g and libc6 were NOT duplicated into the prefix)

$ ld-linux-aarch64.so.1 --list .../root/usr/bin/ciso
	libz.so.1 => /data/data/com.termux/files/usr/glibc/lib/libz.so.1
	libc.so.6 => /data/data/com.termux/files/usr/glibc/lib/libc.so.6
```

Only `ciso`'s own files (the binary, a man page, docs) were unpacked into
`$INSTDIR`. Both of its actual dependencies resolve live against Termux's
existing glibc side-install, found automatically by the dynamic linker
with no `LD_LIBRARY_PATH` or wrapper needed — the glibc `ld.so` living
inside `$PREFIX/glibc` already searches its own prefix by default.

### Real problem found and fixed: epoch comparison

First attempt seeded `Version: 1.3.2` (Termux's own zlib-glibc version
string, no epoch) for `zlib1g`. `ciso`'s real dependency is `zlib1g (>=
1:1.1.4)` — Debian's zlib1g carries epoch `1`. dpkg's version comparison
checks epoch *before* comparing the rest, so `1.3.2` (implicit epoch `0`)
was judged **older** than `1:1.1.4` (epoch `1`) despite being numerically
much newer — configure failed with a real dependency error, not a bug in
the mechanism, a bug in the seed data.

**Fix:** seed every native stub with an artificially high epoch
(`9999:$termux_version`). The claim a stub makes is "this dependency is
functionally satisfied by Termux's own package," not "here is a real,
comparable version number" — Debian epochs are per-library, drift over
time, and aren't worth tracking accurately for a claim that isn't a real
version in the first place. An always-winning epoch makes that claim
directly instead of guessing at Debian's current epoch per library.

### What `native-seed.sh` cannot do yet: the mapping is hand-written

The Debian-name → Termux-package mapping (`libc6:glibc`,
`zlib1g:zlib-glibc`, ...) inside the script is a **static, hand-maintained
table of ~10 entries**, not derived from anything. This is the real
remaining gap:

- Debian splits shared libraries into fine-grained per-soname/per-ABI
  packages (`libssl3`, `libssl3t64`, ...); Termux's `*-glibc` packages
  don't follow that convention at all, so there's no mechanical name
  transform — someone has to know `libssl3` means "the openssl-glibc
  package," and that mapping isn't published anywhere to scrape.
- A more robust approach, not yet built: instead of mapping *names*, map
  *sonames*. Walk every installed `*-glibc` package's files
  (`dpkg -L <pkg>`) for `.so*` files, record each one's `SONAME` (`readelf
  -d` / `objdump -p`), and match a `.deb`'s declared `Depends:` against
  that soname table instead of a hand-written package-name guess. This is
  much closer to what dpkg's own `${shlibs:Depends}` mechanism already
  does upstream (matching library files, not package names) — worth
  reusing that convention instead of inventing a parallel one.
- Until that exists, `native-seed.sh`'s table only covers what's been
  tested by hand (`libc6`, `zlib1g`, and a few other guesses not yet
  verified against a real package — `libssl3`/`libncurses6`/etc. entries
  are unverified, added speculatively).

### Open work

- [ ] Build the soname-based mapping (walk `*-glibc` packages' `.so`
      files, extract `SONAME`, match against a `.deb`'s `Depends:` instead
      of a hand-written name table).
- [ ] Verify the unverified table entries (`libssl3`, `libncurses6`,
      `libreadline8`, ...) against a real `.deb` that needs each one, the
      same way `zlib1g` was verified against `ciso` (not yet done — the
      current table is partly guesswork).
- [ ] Decide what happens when a `.deb` depends on a library Termux's
      glibc side-install does not have at all, and whose Debian `.deb`
      *does* need to be pulled in for real (a genuine second-layer
      install, not a stub) — this project's actual "install into
      `$INSTDIR` for real" path, only exercised so far for a package's
      *own* files, never yet for a transitively-pulled dependency `.deb`.
## Install path: reuse Termux's apt/dpkg, don't fork them


Status: **architecture decision.** Not implemented yet.

### Decision

Do **not** fork or patch apt/dpkg source, unlike sudo-less. Use Termux's
own `apt`/`dpkg` binaries as-is, pointed at a separate prefix via their
existing relocation flags and a custom `apt.conf`.

### Why sudo-less had to fork, and why we don't

sudo-less runs on a real Debian host, where stock dpkg refuses to operate
without root ("requested operation requires superuser privilege"), calls
`chown`, and checks for `ldconfig` on `PATH` at startup. Their patches
(`0001-no-superuser-check`, `0002-no-chown`, `0003-no-ldconfig-check`)
remove exactly those checks — and per `docs/apt-dpkg-port.md` in sudo-less,
those patches are themselves lifted from Termux's own `termux-packages`
patch set, just made unconditional instead of `#ifndef __ANDROID__`.

We're running *in* Termux. Termux's apt/dpkg already ship the
`__ANDROID__`-guarded version of those same changes, built in. There is
nothing left to remove.

### What to do instead

#### dpkg: relocate with existing flags, no chroot

```sh
dpkg --instdir="$NEWPREFIX" \
     --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-script-chrootless \
     --force-not-root \
     -i pkg.deb
```

This is stock dpkg functionality (`--instdir`, `--force-script-chrootless`,
`--force-not-root`), not a patch. It's also exactly what sudo-less itself
used *before* building "the view" (`apt-dpkg-port.md`, point 6: "Before the
view: `--instdir=$PREFIX` and `--force-script-chrootless`").

Consequence of skipping the view (accepted limitation, see
[`design.md`](design.md)): maintainer
scripts run via `--force-script-chrootless` execute directly with Termux's
own `/bin/sh`, seeing real absolute paths (`/etc/foo`) that do **not**
resolve into `$NEWPREFIX` — only `$DPKG_ROOT` (which dpkg exports to
scripts) tells a well-behaved script where the real files are. A script
that assumes `/etc/foo` unconditionally means the prefix will write to (or
fail to write to) the real root instead. This is the same class of gap
Direction 2 already flags, not a new one.

#### apt: point Dir::* at the new prefix, no patch

A dedicated `apt.conf` (not committed to Termux's own):

```
Dir::State "NEWPREFIX/var/lib/apt";
Dir::State::status "NEWPREFIX/var/lib/dpkg/status";
Dir::Cache "NEWPREFIX/var/cache/apt";
Dir::Etc "NEWPREFIX/etc/apt";
RootDir "NEWPREFIX";
```

Loaded via `apt -o Dir::... ` overrides or `APT_CONFIG=path/to/this.conf`.
This mirrors sudo-less's generated `00local-prefix` — a config file, not a
source change.

`$NEWPREFIX` must be **separate from Termux's own `$PREFIX`**
(`/data/data/com.termux/files/usr`) — reusing it would let this project's
installs corrupt Termux's own package database. Use something like
`~/.local` (matching sudo-less's own default) or a dedicated directory.

### The two-layer database: seeding runs backwards here

sudo-less seeds the prefix's dpkg status from the **host's**
`/var/lib/dpkg/status` so apt treats already-present system libraries
(glibc included) as satisfied and only installs leaf packages. Their host
already has glibc — that's the whole premise.

Termux has no glibc at all. For glibc arm64 `.deb`s, we specifically *want*
apt to pull in the full `libc6` dependency chain into `$NEWPREFIX` — that's
the actual point of this project (glibc side-install, same rootfs
glibc-runner has been manually pointing patched ELF interpreters at).

Seeding may still be useful for the *other* direction: packages Termux
already provides a Bionic-native equivalent for (`zlib1g`, `libssl3`,
etc.), where duplicating a glibc copy into `$NEWPREFIX` would be wasted
disk/maintenance for no benefit if the plan ends up being "run glibc
binaries against the glibc side-install's own full lib chain" rather than
"mix and match Bionic and glibc libs" — mixing the two would be an ABI
minefield anyway, so the realistic default is: **don't seed from Termux's
own package set at all**; let `$NEWPREFIX` be a self-contained glibc tree,
and only reconsider seeding if disk footprint or duplicate maintenance
becomes an actual problem.

### Maintainer scripts calling root-only or missing helpers

Same caveat sudo-less documents (`apt-dpkg-port.md`, "Not patched, on
purpose"): scripts calling `ldconfig`, `update-alternatives`, `systemctl`,
`py3compile`, etc. need either a shim on `PATH` inside `$NEWPREFIX/bin` or
the package excluded. Not designed yet — first need a real sample of
maintainer scripts from target packages to see which helpers actually get
called (open item, same as the coverage survey in
`design.md`).

### Open work

- [ ] Confirm Termux's shipped `apt`/`dpkg` versions support all the flags
      above (`--force-script-chrootless` in particular) — check
      `dpkg --version` / `man dpkg` on-device rather than assuming version
      parity with sudo-less's apt 3.3.3 / dpkg 1.23.11.
- [ ] Verify apt's signature verifier: sudo-less relies on the host's `sqv`
      (apt 3.x default); confirm what Termux's apt build uses (`sqv` or the
      older `gpgv`) and that it's present.
- [ ] Decide `$NEWPREFIX`'s location and whether it's user-configurable
      from day one or hardcoded for the R&D phase.
- [ ] Test `dpkg --instdir` + `--force-script-chrootless` against one real,
      simple glibc arm64 `.deb` (e.g. `hello`) end to end before attempting
      anything with maintainer scripts or dependencies.
## Direction 2: static per-binary wrappers (replaces the "view")


Status: **design, not implemented.**

### Problem this replaces

sudo-less's "view" (`docs/view.md` in sudo-less) makes a package's
hardcoded absolute paths (`/etc/foo.conf`, `/usr/share/foo/templates`)
resolve correctly at run time by overlaying the prefix onto the real `/usr
/etc /var /opt` inside a private mount namespace, live, for the duration of
the call. That needs `unshare(CLONE_NEWUSER)` + unprivileged overlayfs,
both assumed blocked under Termux's SELinux domain (see
[`design.md`](design.md)).

### Approach

Do the same job **ahead of time, per binary, with no namespace**: at
install time, for each program `prefix-wrap`'s heuristics flag as needing
path help, generate a fixed script (or patch the binary directly) that
resolves its paths against the prefix explicitly, instead of relying on
`/etc/foo.conf` transparently meaning the prefix's copy.

Concretely, by failure mode (same table as sudo-less's `view.md`):

| why the binary needs help | static fix |
|---|---|
| interpreter shebang not on host (`#!/usr/bin/ruby`, host only has Termux's `ruby`) | rewrite the shebang at install time to the actual interpreter path in the prefix, or wrap with `exec $PREFIX/usr/bin/ruby "$0" "$@"` |
| interpreter only searches compiled-in module paths (Python/Perl/Node/...) | wrapper sets the interpreter's own search-path env var (`PYTHONPATH`, `PERL5LIB`, `NODE_PATH`, ...) to the prefix's copy before `exec`ing — this is exactly the case sudo-less's own docs call out as **not** solvable by env vars *for the view's other cases*, but it's the right tool specifically for module search paths |
| `ldd` can't find a library the package ships | wrapper sets `LD_LIBRARY_PATH` to the prefix's lib dir before `exec` — same caveat as above: fine for this one case, not a general substitute for the view |
| ELF binary is a glibc build, needs glibc-runner | wrapper (or the binary's patched ELF interpreter directly, per the existing manual glibc-runner method) invokes it against the glibc side-install |
| binary/script has a hardcoded absolute path to its own data (`/usr/share/figlet`, `/etc/redis/redis.conf`) it reads directly, not through a library call the above env vars cover | **solved** — see [`design.md`](design.md): our own shim (`native/path-redirect.c`) intercepts `open`/`openat`/`fopen`/`stat`/`fstatat` and rewrites the path, verified against `figlet`'s real `/usr/share/figlet` lookup |

### What this used to not solve (now closed)

The view's whole point is Debian packages assume `/` is real. A static
wrapper only helps for the *specific, enumerable* ways a program looks
things up (interpreter search paths, dynamic linker search paths,
shebangs). A binary that does `open("/etc/foo.conf")` directly in its own
C code, with no env var and no CLI flag to redirect it, used to have no
static fix short of binary-patching the literal path string (only works if
the replacement is the same length or shorter) — **until
`design.md`'s shim, now built and verified**.
Binary-patching the string remains the fallback for a statically-linked
binary (no dynamic libc calls to intercept), which the shim genuinely
cannot reach.

Per sudo-less's own survey (`survey-2026-09.md`, referenced from
`view.md`): ~73% of packages need **no** path help at all and just run from
the prefix's `bin/` directly. Of the remaining ~27%, an unmeasured fraction
falls into the "hardcoded path, no env var" bucket this direction can't
reach. **Needs its own survey against real glibc arm64 `.deb`s before
claiming a coverage number** — do not assume sudo-less's 73%/27% split
transfers; it was measured on Debian's package set with Debian's own
`/usr` layout assumptions, not against Termux's prefix.

See [`design.md`](design.md) for how this detection/generation
step gets triggered automatically after an install (apt's
`DPkg::Post-Invoke` plus a `dpkg` wrapper script, not a patch).

### Open work

- [ ] Run `prefix-wrap`'s detection heuristic (or a reimplementation of it)
      against a real sample of glibc arm64 `.deb`s to get an actual
      coverage number for "wrapper suffices" vs. "needs a path-virtualization
      layer neither direction handles yet".
- [x] ~~Decide whether the shim is worth building~~ — built,
      see `design.md`.
- [ ] Wire the shim's env vars (`DN_REDIRECT_FROM`/`_TO`) into the
      wrapper-script generation this doc describes, instead of setting
      them by hand as done for the `figlet` test.
- [ ] Decide whether the remainder (statically-linked binaries, unreachable
      by any dynamic-call interception) is small enough to just exclude
      (same as sudo-less excludes root-only-maintainer-script packages).
- [ ] Reuse or reimplement `prefix-wrap`'s wrapper-script generation
      (`$PREFIX/bin/<name>` script recorded per-package so it's removed
      with the package) — this part has no namespace dependency and can
      likely be ported near-verbatim.
## Triggering Direction 2's wrapper generation: apt/dpkg hooks, not a patch


Status: **design, not implemented.** Depends on
[`design.md`](design.md) (what gets
generated) and [`design.md`](design.md) (the
prefix layout it runs against).

### Decision

Run the wrapper-generation step (detect which newly-installed binaries
need a shebang rewrite / env-var wrapper / glibc-runner ELF patch, per
`design.md`) automatically after every install, using two
stock hook points — no apt/dpkg source change:

1. **`DPkg::Post-Invoke`** in a prefix-scoped `apt.conf.d` snippet, for
   installs done through `apt-get install`.
2. **A `dpkg` wrapper script** on `$NEWPREFIX/bin`, ahead of the real dpkg
   on `PATH`, for direct `dpkg -i` calls that bypass apt entirely.

This mirrors sudo-less's own mechanism (`docs/view.md`, "How programs get
there"): their `prefix-wrap` runs via `apt.conf.d/02integrate.in`
(`DPkg::Post-Invoke`-style) after apt-driven installs, and via their own
`$PREFIX/bin/dpkg` wrapper "after a dpkg run apt did not make" — i.e. they
hit the same gap and solved it the same way.

### Why both are needed (the gap that breaks if only one exists)

- `DPkg::Post-Invoke` is an **apt** config directive, honored only when
  **apt** invokes dpkg. It does nothing for a bare `dpkg -i pkg.deb`
  typed directly, or run by some other script — dpkg has no equivalent
  generic "ran to completion" hook of its own (dpkg *triggers* exist, but
  they're package-declared interest in specific paths, not a general
  post-run hook, and would need the packages themselves to declare
  interest in something they have no reason to know about).
- A `$NEWPREFIX/bin/dpkg` wrapper that shells out to the real dpkg and then
  runs the same detection step closes that gap, but only if it stays ahead
  of the real dpkg on `PATH` — the ordering this project already relies on
  for `$NEWPREFIX/bin` (see `design.md`).
- Neither alone is sufficient: apt-driven installs go through both apt
  *and* the dpkg it calls internally, so the dpkg wrapper's own hook logic
  needs a guard against double-running (an env var set by the apt hook's
  caller, or a lock/marker file for "already ran for this transaction") —
  otherwise a single `apt-get install` would trigger wrapper generation
  twice, redundant but not obviously wrong, so worth avoiding cleanly
  rather than leaving as a known-harmless quirk.

### Config sketch

```
# $NEWPREFIX/etc/apt/apt.conf.d/90wrap-glibc
DPkg::Post-Invoke {
    "test -n \"$DN_DPKG_WRAPPER_RAN\" || $NEWPREFIX/lib/deb-native/wrap-new-binaries";
};
```

```sh
#!/bin/sh
# $NEWPREFIX/bin/dpkg — wrapper, not a patch
export DN_DPKG_WRAPPER_RAN=1
"$NEWPREFIX/lib/deb-native/real-dpkg" "$@"
status=$?
"$NEWPREFIX/lib/deb-native/wrap-new-binaries"
exit "$status"
```

(Names/paths illustrative — not yet decided where the real dpkg binary
gets moved to so the wrapper can claim the `dpkg` name on `PATH`, likely
`$NEWPREFIX/lib/deb-native/real-dpkg` or similar, matching how
sudo-less relocates its own wrapped binary.)

### What "wrap-new-binaries" needs to know

To avoid re-scanning every binary in the prefix on every invoke, it needs
to know what changed since the last run — sudo-less's `prefix-wrap` scopes
itself to "each package installed or changed since the last run" via a
stamp file. Same approach here: compare `$NEWPREFIX/var/lib/dpkg/status`
mtime (or a package-list diff) against a stamp under
`$NEWPREFIX/.deb-native/wrap.stamp`, matching sudo-less's
`$PREFIX/.sudo-less/view/mirror.stamp` pattern referenced in `view.md`.

### Open work

- [ ] Confirm `DPkg::Post-Invoke` fires reliably when apt's `Dir::*` is
      pointed at `$NEWPREFIX` via the custom `apt.conf` from
      `design.md` — not yet tested on-device.
- [ ] Decide the double-run guard mechanism (env var vs. lock file vs.
      transaction id) once the wrapper script itself exists to test
      against.
- [ ] Decide where the real dpkg binary lives once wrapped (can't keep the
      name `dpkg` in the same directory as the wrapper).
## Direction 3 (research): services without systemd


Status: **research only, no design yet.** This doc records what's known and
what's still open — not a plan to implement against.

### What sudo-less does

`docs/services.md` in sudo-less (not yet read in full — follow-up) covers
translating a package's systemd unit into a `systemd --user` unit that runs
inside a "service view": a private mount-namespace view built fresh per
start, so the daemon reads its config from `/etc/foo/foo.conf` and writes
state to `/var/lib/foo` exactly as it would on real Debian, landing in the
prefix. `$XDG_RUNTIME_DIR/sudo-less/run` stands in for `/run`.

### Why it doesn't transfer

Two independent blockers, not one:

1. **No systemd on Termux at all** — no PID 1 systemd, no `systemctl`, no
   user manager, no unit files, no cgroups in the relevant sense. Termux's
   native service supervisor is [`termux-services`](https://github.com/termux/termux-services),
   built on `runit`: services are directories under
   `$PREFIX/var/service/<name>/run` (an executable script `runsv`
   supervises), enabled/disabled via `sv-enable`/`sv-disable`.
2. **No service view** — even if a runit script stood in for the systemd
   unit, the config/state/`/run` redirection sudo-less's service view
   provides depends on the same blocked mount-namespace mechanism as
   Direction 2 is working around. A runit script alone doesn't solve path
   resolution.

### Open questions (not yet answered)

- Does a `.deb` package that ships a systemd unit under
  `/lib/systemd/system/*.service` carry enough information in that unit
  file (`ExecStart=`, `ExecStop=`, `User=`, environment) to mechanically
  generate a runit `run` script, the way sudo-less mechanically generates a
  user unit? Needs reading actual systemd unit files from real packages to
  judge — not researched yet.
- Whether Direction 2's static wrappers (env vars for config/data paths)
  are enough for a *daemon* the same way they might be for a CLI tool, or
  whether daemons disproportionately fall into the "hardcoded absolute
  path, no env var" bucket `design.md` flags as unsolved —
  daemons reading `/etc/<name>/<name>.conf` directly is extremely common
  and was one of `view.md`'s own motivating examples (`redis.conf`). If so,
  Direction 3 may be blocked on the same open problem as Direction 2's
  gap, not just on "no systemd".
- Whether `termux-services`/runit's process supervision model (long-running
  foreground process under `runsv`, restart on exit) is even the right fit
  for packages that assume `systemd`'s notify/socket-activation protocols
  (`Type=notify`, `sd_notify()`) — a package relying on those would need
  either a shim for `sd_notify` or exclusion, unresearched which is more
  common.

### Non-conclusion

This direction is **not** "port sudo-less's service view to runit" — that
undersells the problem, since the actual hard part (path resolution for a
daemon that expects `/etc/foo` and `/var/lib/foo` to be real) is shared
with Direction 2's unsolved gap, not something runit vs. systemd changes
either way. Next step is reading sudo-less's `docs/services.md` in full and
picking one real daemon package to trace by hand (what paths does it read,
what would a runit script + Direction 2's wrapper actually cover) before
writing any design here.
## Prior art: what carries over from sudo-less


Source: [`jronminh/sudo-less`](https://github.com/jronminh/sudo-less),
docs read 2026-09-25 (`README.md`, `docs/porting.md`,
`docs/apt-dpkg-port.md`, `docs/view.md`).

### What sudo-less actually is

Real `apt`+`dpkg`, forked from **Termux's own patches** to those two
projects (`termux/termux-packages`), retargeted from Android/Bionic back
onto a regular rootless Debian host, installing into `~/.local`. Three
Debian-native mechanisms do the work: unprivileged user+mount namespaces,
unprivileged overlayfs (kernel ≥5.11), and `systemd --user`.

The irony worth noting: sudo-less took Termux's patches *out* of the
Android context to make them work on desktop Debian. This repo needs to
partly reverse that back onto Android — but Termux's own apt/dpkg already
covers the parts sudo-less had to rebuild, so that part doesn't need
touching at all.

### Reusable as-is (~40%)

1. **apt/dpkg running root-less in a prefix.** Termux's apt/dpkg already do
   this natively (`$PREFIX=/data/data/com.termux/files/usr`), no fork
   needed. sudo-less's patches (`0001-no-superuser-check`, `0002-no-chown`,
   `0003-no-ldconfig-check`) are literally Termux's own Android-guarded
   changes made unconditional — Termux already has the unconditional
   Android version.

2. **Two-layer package database.** sudo-less seeds the prefix's dpkg status
   from the host's `/var/lib/dpkg/status` so system libraries count as
   "already installed" and apt doesn't try to pull the whole `libc6` chain.
   Direct analogue here: treat Termux's own installed package set as the
   read-only lower layer, so a glibc `.deb` install only pulls the leaf
   packages actually missing (glibc runtime itself, plus whatever the
   package needs that Termux doesn't ship in Bionic form).

3. **`prefix-wrap`'s detection heuristics** (`docs/view.md`, "How programs
   get there" table) — a binary needs help resolving its own paths when
   (first match): it's a symlink leaving the prefix, its interpreter
   shebang isn't on the host, its interpreter only searches compiled-in
   module paths (Python/Perl/Ruby/Node/...), `ldd` can't find one of its
   libraries, or it names a file/dir of its own under `/usr`, `/etc`,
   `/opt` that the prefix has. This detection logic is independent of *how*
   the fix gets applied (mount namespace vs. static wrapper) — reusable
   directly for Direction 2.

### Blocked on Android (~60%)

1. **The "view"** (`docs/view.md`): a private mount namespace overlaying
   the prefix onto `/usr /etc /var /opt`, live, so absolute paths compiled
   into a binary resolve against the prefix. Needs `unshare(CLONE_NEWUSER)`
   + unprivileged overlayfs in that namespace. **Confirmed blocked on this
   device, with the actual syscall error** (see
   `design.md`): `unshare(CLONE_NEWUSER)` fails with
   `EINVAL` (not `EPERM`) — the kernel itself doesn't support unprivileged
   user namespaces at all here, not merely an SELinux policy denial. Plain
   `unshare(CLONE_NEWNS)` alone fails with `EPERM` as expected (needs
   `CAP_SYS_ADMIN`). FUSE is also closed (`/dev/fuse`: permission denied,
   no `fusermount`). Replaced by our userspace path-redirect shim
   shim instead — see `design.md`, verified working against
   a real package (`figlet`).

2. **`prefix-sandbox`'s seccomp/namespace-based isolation** — depends
   entirely on the view above, so it goes with it.

3. **Services via `systemd --user`** — Termux has no systemd at all (no
   init, no cgroups in the relevant sense, no user manager). Native
   equivalent is `termux-services` (runit). See
   [`design.md`](design.md).

### Formerly-open question, now answered

~~Whether `unshare(CLONE_NEWNS)` alone... is available to Termux without
root.~~ Answered: no — `EPERM`, confirmed by direct test and strace. See
`design.md` for the full data and the replacement mechanism
(userspace path redirection, no mount involved at all).

### Related work found later: `proroot` (closed-source)

[`coderredlab/proroot`](https://github.com/coderredlab/proroot) — a
proprietary "drop-in `proot` replacement, zero ptrace overhead" for
Android, 82 stars as of 2026-09. Its README (source not published) hints
at the same class of mechanism as this repo's `design.md`:
separate `libproroot-linker.so` / `libproroot-stub-loader.so` /
`libproroot-bridge.so` components suggest dynamic-linker/libc-call
interception rather than `proot`'s `ptrace`-based one — and its own notice
says "similar LD_PRELOAD-based tools have started appearing recently",
i.e. this general direction (no ptrace, no kernel privilege) is an
independently-emerging category, not unique to this repo.

Two real differences from this project's approach, worth naming plainly:

- **`proroot` fakes `uid=0`/`gid=0`** (`-0` flag, "proot-compatible
  fakeroot"). This repo deliberately does *not* — see the
  fakeroot/no-fakeroot discussion this doc's design decisions follow
  (`design.md`): dpkg's own root-checks are removed rather
  than faked, so files land owned by the real unprivileged uid, not a
  faked `root:root`. Trade-off, not a strict improvement: a package that
  genuinely checks ownership would be fooled by `proroot`'s fake identity
  and not by this project's real one.
- **`proroot` brings a whole guest rootfs** (tested against a full Ubuntu
  arm64 glibc rootfs — Node, Python, Chromium, git), matching `proot`'s
  own scope. This project's approach is deliberately lighter: no full
  rootfs, no bundling what Termux's glibc side-install already provides
  (`design.md`). The cost of that lightness is coverage —
  `proroot`'s full rootfs presumably "just works" for far more packages
  out of the box, where this project has to be more selective about which
  packages it can actually get working (per the low real success rate in
  `findings.md`) rather than getting broad compatibility
  for free.

Being closed-source, `proroot` can't be inspected or reused directly —
noted here as prior art / validation of direction, not a dependency.


## Delivering the shim — "mimicking preload"

The preload environment variable is only one way to get the shim loaded ahead
of libc; the effect we actually need is **symbol interposition**. For a
dynamically-linked glibc target there are four ways, in order of how
baked-in they are:

1. **Environment preload** — `LD_PRELOAD=$SHIM`, what the launchers set today.
2. **Explicit loader call** — `$GLIBC/lib/ld-linux-aarch64.so.1 --preload $SHIM /prog`.
   The same effect with nothing in the environment to scrub; Termux already
   launches glibc programs through an explicit interpreter.
3. **Baked into the ELF** at install time — e.g.
   `patchelf --add-rpath $SHIMDIR --add-needed libpath-redirect.so /prog`,
   so the loader loads the shim on every run regardless of the environment.
4. **`DT_AUDIT` / `DT_DEPAUDIT`** — a loader audit module whose `la_symbind`
   returns an alternative symbol address. It is the loader's supported
   interposition hook and can likewise be patched into the ELF.

Boundary, and the reason a syscall layer is still needed: a **static binary
has no dynamic symbols and no loader** — its libc is compiled in — so none of
the above reaches it. The same is true of raw `syscall()` and libc-internal
calls such as `dlopen`. Those need a syscall-level tracer
(`ptrace` / `SECCOMP_RET_USER_NOTIF`) inside the app uid, which the platform
probe shows is available (`docs/findings.md`, "Platform sandbox limits").
