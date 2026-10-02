# Findings: `apt install gcc` end to end -- two real bugs, one confirmed gap (2026-10-01)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing — a new spec
> topic, a new one-off investigation, or a new guide — not just a
> long addition to what a doc already covers.

**Impact: Open gap.** Two real bugs found and fixed (apt's `PATH` for
maintainer scripts; stand-ins never held), one gap confirmed as a real,
known limitation rather than a bug — but Bug 1 (a bootstrapped prefix
breaking when `scripts/` moves underneath it) is explicitly **not**
fixed as an architectural issue, only worked around for this session.

Testing the Alpha goal's "Compilers" item directly (`apt install gcc`,
compile, run) rather than reasoning from the glibc-build groundwork alone
surfaced two real bugs immediately, both fixed, plus a confirmation of an
already-known packaging gap.

## Contents

- [Bug 1: a bootstrapped prefix breaks when scripts/ moves](#bug-1-an-already-bootstrapped-prefix-breaks-when-the-repos-scripts-layout-changes-underneath-it)
- [Bug 2: apt-driven installs never see $DN/usr/bin on PATH](#bug-2-the-real-find-apt-driven-installs-never-see-dnusrbin-on-path-for-maintainer-scripts-so-anything-only-shipped-there-fails-not-found)
- [Confirmed, not a bug: libc6-dev has no installation candidate](#confirmed-not-a-bug-libc6-dev-has-no-installation-candidate)
- [Found checking the above, a real gap, fixed: stand-ins were never held](#found-checking-the-above-a-real-gap-fixed-libc6dpkgapt-stand-ins-were-never-dpkg-hold-ed)

## Related docs

- [`libc6-dev-gap-closed.md`](libc6-dev-gap-closed.md) — the direct
  follow-up closing the "confirmed, not a bug" item below.
- [`gcc-hello-pt-interp-gap.md`](gcc-hello-pt-interp-gap.md) — the next
  entry in the same compiler-toolchain investigation.
- `TODO.md` — Bug 1's open item lives there, not here.

## Bug 1: an already-bootstrapped prefix breaks when the repo's `scripts/` layout changes underneath it

`setup-apt-prefix.sh` writes the prefix's
`apt.conf` once, at bootstrap time, with absolute paths to the exact
checkout state at that moment (`DPkg::Pre-Install-Pkgs`,
`APT::Update::Post-Invoke-Success`, ...). Nothing regenerates it for an
existing prefix -- `install.sh`'s "refreshing the existing prefix" branch
calls `setup-runtime.sh`/`make-launchers.sh`/`make-apt-wrappers.sh`/
`dn-activate.sh`, never `setup-apt-prefix.sh` again. A prefix bootstrapped
before this session's `scripts/` reorg (flat -> `bootstrap`/`install`/
`runtime`/...) had its hooks pointing at paths that no longer exist;
`apt update` failed outright (`Post-Invoke-Success` script not found).
Not unique to this reorg -- any future script move hits the same wall for
anyone who already has a prefix. **Worked around for this test session by
deleting and re-bootstrapping; not fixed as an architectural issue** --
see `TODO.md` for the open item (candidates: have `termux-dn-doctor`
detect and rewrite stale hook paths, or have the "refresh" path also
rewrite just the hook lines of an existing `apt.conf`).

## Bug 2 (the real find): apt-driven installs never see `$DN/usr/bin` on `PATH` for maintainer scripts, so anything only shipped there fails "not found"

`gcc`'s dependency `cpp` failed installing with `preinst: 4:
dpkg-maintscript-helper: not found`, exit 127 -- `dpkg-maintscript-helper`
is a plain POSIX-sh script `dn-standins.sh`'s `dpkg` stand-in already
extracts from Debian's real `dpkg` `.deb` into `$DN/usr/bin` (not a new
gap; already handled, correctly). Root cause, found by instrumenting a
copy of `cpp`'s actual `preinst` with `set -x; env` and installing it
directly: apt forks dpkg via its own compiled-in `Dir::Bin::dpkg` default
(Termux's real `/usr/bin/dpkg`), **not** the project's own
`launcher()`-generated `$DN/usr/bin/dpkg` wrapper that sets
`PATH="$DN/usr/bin:$DN/usr/sbin:$PATH"` before exec'ing it -- confirmed by
`apt-config dump` showing `Dir::Bin::dpkg
"/data/data/com.termux/files/usr/bin/dpkg"` with no override in the
prefix's own `apt.conf`. Running the exact same `.deb` through
`$DN/usr/bin/dpkg` directly (not via apt) succeeds -- same package, same
maintainer script, the only difference is which process tree invoked it.

**Fix:** `DPkg::Path "$DN/usr/bin:$DN/usr/sbin:$TP/bin";` in the prefix's
own `apt.conf` (`setup-apt-prefix.sh`). Confirmed empirically (Termux's
own apt already ships a `DPkg::Path` default pointing at its own
`$PREFIX/bin`, which is what led to testing this key rather than
`Dir::Bin::dpkg` itself) -- `apt-config dump` shows it's read independent
of `Dir::Bin::dpkg`, and a full `apt install gcc` after adding it unpacks
and configures `cpp`/`gcc` and every other dependency with zero manual
intervention, reproduced from a fresh bootstrap.

## Confirmed, not a bug: `libc6-dev` has no installation candidate

`apt install libc6-dev` fails outright ("no installation candidate") --
Debian's real `libc6-dev` is versioned against stock `libc6 (=
2.41-12+deb13u4)`, an exact-version `Depends`, and this test prefix's
`libc6` is `dn-standins.sh`'s stand-in, `2.44-0dn1` (Termux's own glibc
version, a fresh bootstrap still defaults to the stand-in, not 0.5.0's
real own-glibc build -- `TODO.md`). Either way the version string can
never equal what Debian's archive demands, by construction -- a `+dn1`
suffix on the real build would fail the same exact-match check. This is
`TODO.md`'s already-known "package the rest: `libc6-dev`/`libc-bin`/
`locales`" item, now confirmed as the actual, reproducible blocker on
`gcc -c hello.c` (`stdio.h: No such file or directory`) rather than an
inferred one -- `gcc`/`cpp`/`binutils` themselves install and configure
cleanly; only the headers are missing.

## Found checking the above, a real gap, fixed: `libc6`/`dpkg`/`apt` stand-ins were never `dpkg hold`-ed

Only `setup-apt-prefix.sh`'s
`$BASE` set gets `dpkg --set-selections hold`; the three stand-ins
(installed earlier, by `dn-standins.sh`) did not. An `apt upgrade` would
have been free to replace `libc6` with Debian's real one -- which
segfaults at startup on this device (`patchelf-et-exec-runpath.md`, and
`android-seccomp-audit.md`'s "stock Debian `libc6` doesn't
even reach a syscall question") -- or `dpkg`/`apt` with Debian's real
ones, which don't work in this environment at all (`design.md`,
"Prior art"). Fixed in `dn-standins.sh`'s shared `install_pkg()`, so
every current and future stand-in installed through it is held
automatically. Verified: fresh bootstrap, all three show `hold ok
installed`; `apt full-upgrade -y --dry-run` no longer proposes touching
any of them.
