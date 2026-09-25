# Findings: wiring real apt — 2/30 → 10/30 (2026-09-25)

Direct follow-up to `findings-survey-2026-09-25.md`, which found the
dominant failure cause (27/30) was that this project never actually
installed a package's dependencies — `prototype-install.sh` only unpacked
the one `.deb` it was given. This round builds and tests the fix: real
`apt`, pointed at a real Debian repo, scoped to a separate prefix.

## The fix: `scripts/setup-apt-prefix.sh` + `scripts/apt-install.sh`

`setup-apt-prefix.sh` generates a prefix-scoped `sources.list` (real
`deb.debian.org`) and `apt.conf` (`Dir::*` into the prefix, `Dpkg::options::`
carrying the `--instdir`/`--admindir`/`--force-*` flags every dpkg
invocation apt makes now needs — previously set by hand per call in
`prototype-install.sh`), runs `native-seed.sh`, then `apt-get update`.
`apt-install.sh` runs `apt-get install` and patches every new ELF with
`grun --configure` afterward. Verified first against `libbs2b0` (the exact
package that failed with `depmissing` in the first survey, needing
`libstdc++6`): apt now pulls `gcc-14-base`, `libgcc-s1`, `libstdc++6` for
real and all four configure successfully.

**Known insecure shortcut, not fixed yet:** `sources.list` uses
`[trusted=yes]` — Termux ships no Debian archive keyring, so signature
verification is skipped entirely rather than solved.

## Same 30-package sample, same seed, real result: 10/30 (33%)

| outcome | count |
|---|---|
| `ok` | 10 |
| `unresolvable` | 10 |
| `script` | 6 |
| `unpack` | 4 |

Up from 2/30 (7%) with bare `dpkg`. Real progress, and close to (though
short of) the "even 40% would be great" bar set going in — with two new,
previously-unseen failure categories now dominant instead of `depmissing`.

## New finding: the `LD_PRELOAD` shim doesn't reach maintainer scripts

Two of the 6 `script` failures are the *same class* of problem
`design-manual-overlay.md` already solved for compiled binaries —
a hardcoded absolute path with nothing there to find:

```
debconf.postinst[5]: .: /usr/share/debconf/confmodule: No such file or directory
...
ln: failed to create symbolic link '/usr/lib/ssl': No such file or directory
```

But `native/path-redirect.c`'s `LD_PRELOAD` shim **cannot help here**, for
a reason not previously documented: maintainer scripts run under
`--force-script-chrootless` execute via **Termux's own Bionic `/bin/sh`**,
not a glibc process. `LD_PRELOAD=path-redirect.so` names a glibc `.so`
built against Termux's glibc side-install — it does not load into (and
would likely be rejected by) a Bionic dynamic linker at all. The manual
overlay's actual scope is narrower than it may have read before: **glibc
dynamically-linked binaries only**, not shell maintainer scripts. A
Bionic-targeted build of the same interception idea would be a separate,
unbuilt piece of work if this gap is worth closing.

This matches sudo-less's own survey finding almost exactly (`survey-2026-09.md`,
"maintainer script failures (13%) fall into known classes", including
"writes to `/etc`" and "calls the package's own binary by absolute path")
— same underlying class of problem, worse here because Android has no
real `/usr`/`/etc` tree at all backing those paths (sudo-less's host at
least has a real, if root-owned, one).

## New finding: dpkg's intra-package hardlinks fail on this filesystem

All 4 `unpack` failures are the same error shape:

```
dpkg: error processing archive .../perl-base_5.40.1-6+deb13u1_arm64.deb (--unpack):
 error creating hard link './usr/bin/perl5.40.1': Permission denied
```

Some Debian packages ship two files as hard links to the same inode (space
saving in the `.deb`); dpkg recreates the hard link on unpack. This isn't
sudo-less's `0103-link-or-copy.patch` case (that's for *backing up a host
file* dpkg is about to replace, unrelated) — this is a hard link **within
the package being unpacked**, onto a filesystem/path
(`.cache/claude-tmp/.../scratchpad/...`, deep under Termux's own storage)
that refused it. Not yet determined whether this is inherent to the
storage backing this specific path, or a more general Android/Termux
filesystem limitation — needs testing on a plain path directly under
`$HOME` before concluding either way.

## `unresolvable`: apt's solver, working as intended, hitting a real limit

```
Depends: libc-dev-bin (= 2.41-12+deb13u4) but it is not going to be installed
```

Several `-dev`/`-dbg` packages want an exact-version real `libc6-dev`-family
package installed for real, unrelated to `native-seed.sh`'s stub (which
only claims plain `libc6` is satisfied, not the `-dev` headers/tools
package). This is apt correctly refusing to fake a real, versioned
build-time dependency — not a bug, a genuine current limitation: this
project has never tried actually installing such a package's *own* real
`.deb`, only ever stubbing it.

## What changes because of it

- The install-path work (`design-install-path.md`) should be updated:
  `apt` is now real and tested, not just planned.
- `design-manual-overlay.md` needs a scope correction: its `LD_PRELOAD`
  shim only reaches glibc-dynamically-linked binaries, not maintainer
  scripts run via Termux's Bionic `/bin/sh` — a real, previously-unstated
  limitation.
- Next survey round candidates, roughly in order of likely payoff: (1)
  retest the `unpack`/hardlink failures on a shallow path directly under
  `$HOME` to rule out a path-depth/storage artifact; (2) look at whether
  `dpkg --force-hard-link`-equivalent or copy-instead-of-link is needed
  (a real dpkg patch, or a documented limitation to accept); (3) decide
  whether the `script` failures are worth a Bionic-side interception layer
  or should just be an accepted exclusion class (sudo-less's own approach
  for root-only maintainer scripts).
