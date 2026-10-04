# Native dependency reuse

<!-- template: templates/docs.template.md -->

sudo-less's "host already has it" idea, adapted for a host (Termux)
that has no glibc at all: how `native-seed.sh` decided a Debian
dependency was already satisfied by one of Termux's own `*-glibc`
packages, instead of duplicating it into the prefix. Split out of
[`design.md`](../spec/design.md).

**Superseded, not current.** `native-seed.sh` is gone (see
[`design.md`](../spec/design.md), "What went away from the classic design"): the
0.2.0 pivot's real `libc6` stand-in satisfies these dependencies directly,
so there is no stub database to seed any more. Kept as the record of the
idea and its one real gap (the hand-written name mapping below) — `TODO.md`
treats a soname-based version of this same idea as a possible future
improvement, not a revival of this file.

## Contents

- [The idea, restated for this project](#the-idea-restated-for-this-project)
- [How it worked: `native-seed.sh`](#how-it-worked-native-seedsh)
- [Verified end to end against a real package with a real dependency](#verified-end-to-end-against-a-real-package-with-a-real-dependency)
- [Real problem found and fixed: epoch comparison](#real-problem-found-and-fixed-epoch-comparison)
- [What `native-seed.sh` never did: the mapping was hand-written](#what-native-seedsh-never-did-the-mapping-was-hand-written)
- [Where this idea goes next](#where-this-idea-goes-next)

## Related docs

- [`design.md`](../spec/design.md) — the doc this was split out of.
- [`classic-design.md`](classic-design.md) — the two-layer-database idea
  this doc builds on and corrects.
- [`../log/findings/hard-package-ruby-adsf.md`](../log/findings/hard-package-ruby-adsf.md)
  — where the epoch-comparison bug was found.
- [`../log/findings/first-working-prototype.md`](../log/findings/first-working-prototype.md)
  — where the native-mapping gap was originally found.

## The idea, restated for this project

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
own collection point (`$INSTDIR`, the default prefix) —
that's the one place matching sudo-less's `.local` role, but scoped to the
actual delta instead of everything.

## How it worked: `native-seed.sh`

Before unpacking a `.deb`, seed `$ADMINDIR/status` with synthetic
`Status: install ok installed` stanzas for Debian package names already
covered by an installed Termux `*-glibc` package — no files copied, dpkg
just treats the dependency as met. dpkg's own "no files list" handling
(already a normal code path, used for essential packages with no tracked
file list) takes over gracefully; verified in practice, not just assumed:
dpkg prints a warning ("files list file... missing; assuming package has
no files currently installed") and proceeds normally.

## Verified end to end against a real package with a real dependency

`ciso` (`ciso_1.0.2-2+b1_arm64.deb`, from `deb.debian.org`) — a genuine,
tiny CLI tool (PSP ISO↔CSO converter) whose only dependencies are `libc6`
and `zlib1g`. Chosen specifically because it's a real-world case that
*needs* this native-reuse mechanism, not the simplest possible case
(`hello`, tested first, has no non-libc dependency at all).

Historical transcript (`prototype-install.sh`, since removed; see
[`classic-design.md`](classic-design.md)):

```
$ scripts/survey/prototype-install.sh ciso.deb
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

## Real problem found and fixed: epoch comparison

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

## What `native-seed.sh` never did: the mapping was hand-written

The Debian-name → Termux-package mapping (`libc6:glibc`,
`zlib1g:zlib-glibc`, ...) inside the script was a **static, hand-maintained
table of ~10 entries**, not derived from anything. This was the real gap,
and it's the part of the idea `TODO.md` still considers worth revisiting:

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
- The table only ever covered what had been tested by hand (`libc6`,
  `zlib1g`, and a few other guesses never verified against a real
  package — `libssl3`/`libncurses6`/etc. entries were unverified, added
  speculatively).

## Where this idea goes next

Not open work on this file — it's gone. If the soname-based mapping idea
above is ever built, it's a new mechanism inspired by this one, not a
revival of `native-seed.sh` itself (the 0.2.0 pivot's real `libc6`
stand-in means there's no stub database left to improve). See `TODO.md`'s
"Soname-based dependency matching" item for the current framing of that
possible future work.
