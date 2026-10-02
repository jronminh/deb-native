# Findings: closing the `libc6-dev` gap -- no version bump, not a custom package (2026-10-01)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing — a new spec
> topic, a new one-off investigation, or a new guide — not just a
> long addition to what a doc already covers.

**Impact: Repo change.** The real fix was removing a version-bump
suffix, not building a custom package (the first approach tried was
abandoned as the wrong direction). Nothing left open.

Following up the "confirmed, not a bug" item in
[`apt-install-gcc-end-to-end.md`](apt-install-gcc-end-to-end.md): first tried packaging a
custom `libc6-dev` too (own build's headers/static libs over Debian's real
package as a template, same technique as `dn-package-glibc.sh`). Got it
working -- built, installed, `gcc -c` found `stdio.h` -- but this is the
wrong direction: it only moves the exact-version-match wall one package
down the dependency graph (`libc-dev-bin`, then whatever depends on
*that* at an exact version, indefinitely -- a patch chain with no natural
end).

## Contents

- [Real fix: don't version-bump the custom libc6 build at all](#real-fix-dont-version-bump-the-custom-libc6-build-at-all)

## Related docs

- [`apt-install-gcc-end-to-end.md`](apt-install-gcc-end-to-end.md) — the
  entry this one directly follows up on.

## Real fix: don't version-bump the custom `libc6` build at all

`dn-package-glibc.sh` previously appended `+dn1` to the version string
pulled from the real Debian `.deb` template; removed. Our own build is
the *same* upstream source plus the *same* Debian patch series plus one
more patch that only changes what's needed to run under Android's
seccomp filter (`third_party/glibc-android-patches/`) -- it doesn't stop
being "glibc 2.41-12+deb13u4", so claiming a different version was never
accurate, and it was the only thing standing between `libc6-dev`'s
`Depends: libc6 (= 2.41-12+deb13u4)` and a true match. With the suffix
gone, Debian's real `libc6-dev`/`libc-dev-bin` install **unmodified**,
straight from the archive -- no custom packaging script needed for either
(deleted the one just built). Held via the same mechanism as `libc6`
itself (`dn-standins.sh`/this script's caller), so `apt upgrade` can't
silently swap it for Debian's real, unpatched `libc6` later.

That alone wasn't enough: `apt install libc6-dev` still failed with "no
installation candidate" even after rebuilding `libc6` without the suffix.
Cause: `setup-apt-prefix.sh`'s `write_pins()` pins `libc6-dev:arm64` and
`libc-dev-bin:arm64` to priority -1 against both Debian mirrors -- a
leftover from when the plan was still "patch every related package",
written before this version-match approach existed. Removed both names
from the `PINNED` list (and hand-patched the live test prefix's existing
`etc/apt/preferences.d/deb-native` to match, since pins are written once
at bootstrap). `libc-bin`/`libc-l10n`/`locales` stay pinned for now --
same reasoning likely applies once `libc6`'s version matches, but
untested, and `libc-bin` ships `ldconfig`, which has its own known
seccomp quirk (`TODO.md`, 0.5.0) worth checking on its own before
assuming it's as simple as `libc6-dev` turned out to be.
