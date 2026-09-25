# Findings: first working prototype (2026-09-25)

Tested by hand on-device against a real package from `deb.debian.org`
(`hello_2.10-5_arm64.deb`, no maintainer scripts — chosen deliberately as
the simplest possible case). Script: `scripts/prototype-install.sh`.

## Result: it works, end to end

```
$ scripts/prototype-install.sh hello.deb
==> unpacking hello into ~/.termux-deb-bridge/root
==> configuring hello (dependency check bypassed — prototype only)
==> patching new ELF binaries with grun --configure
   patched: /usr/bin/hello
==> done.
$ ~/.termux-deb-bridge/root/usr/bin/hello
Hello, world!
```

A real, unmodified Debian arm64 binary, from Debian's own archive, running
on Termux, installed through Termux's own unpatched `dpkg`.

## Major finding: Termux already ships a curated glibc side-install

Not previously known when `design-install-path.md` and
`design-static-wrappers.md` were written (both assumed Termux "has no
glibc at all" — **that assumption was wrong**, correcting it here):

- Termux has an official `glibc` package repo
  (`termux-pacman/glibc-packages`), installed via the *same* `apt`/`pkg`
  already on the system — packages named `<name>-glibc` (`bash-glibc`,
  `coreutils-glibc`, `openssl-glibc`, ...), landing under
  `$PREFIX/glibc/...` (a real `bin`, `etc`, `include`, `lib` tree).
- `glibc-runner` (`grun`) is a Termux package that **already automates the
  ELF-interpreter-patch step** this whole project calls "Direction 2" /
  "method #2": `grun --configure ./some-glibc-binary` patches the ELF's
  `.interp` to point at `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` in
  place, permanently — after that the binary runs directly, no wrapper
  needed. Verified: `readelf -p .interp` on the patched binary shows the
  rewritten path.
- **This means Direction 2's "ELF-patch" case
  (`design-static-wrappers.md`'s table, row "glibc-runner") is already
  solved, by Termux itself.** Nothing to build there beyond calling `grun
  --configure` on newly-installed ELF binaries — which is what
  `prototype-install.sh` does.

Consequence: this project's actual job shrinks to "get a real Debian
`.deb`'s files unpacked into a place `grun`-patched binaries can find their
libraries," not "build a whole path-virtualization system." Re-scope
`design-static-wrappers.md`'s remaining open problem (hardcoded
`/etc`/`/usr`-internal paths with no env var) — that part is still real and
still unsolved, but it's a much smaller remaining surface than assumed.

## Two real blockers found, both worked around, neither properly solved yet

### 1. Architecture name mismatch: `arm64` vs `aarch64`

```
dpkg: error processing archive hello.deb (--install):
 package architecture (arm64) does not match system (aarch64)
```

`dpkg --print-architecture` on Termux reports `aarch64` (its own Bionic
build's convention), but Debian's `.deb`s declare `Architecture: arm64`
(Debian's own triplet naming for the same CPU). dpkg does a literal string
compare, not a CPU-equivalence check, so it refuses.

**Workaround used:** `--force-architecture`. **Not a real fix** — this
flag disables the check globally for the invocation, including for an
actually-wrong architecture (a real x86_64 `.deb`, say). A real fix would
teach dpkg's admindir that `arm64` is this system's architecture (e.g. a
dedicated `$ADMINDIR` whose `dpkg --print-architecture` — which is
compiled in, not admindir-configurable in stock dpkg — would need a
config override; needs research, not yet done, into whether dpkg has any
non-source-patch way to do this, or whether `--force-architecture` scoped
to only the `$ADMINDIR` context, i.e. accepted as a permanent property of
this project's install path rather than a blanket flag, is the realistic
answer).

### 2. Empty admindir has no dependency chain registered

```
dpkg: dependency problems prevent configuration of hello:arm64:
 hello:arm64 depends on libc6 (>= 2.38).
```

A fresh, separate `$ADMINDIR` (deliberately kept apart from Termux's own,
per `design-install-path.md`) starts with zero packages registered, so
*every* dependency looks missing — even though `$PREFIX/glibc` likely
already satisfies `libc6` in practice (Termux's own `glibc` package is
2.44, well past `>= 2.38`).

**Workaround used:** `--force-depends`. **Not a real fix, and unsafe as a
default** — this bypasses dependency checking entirely, for every
dependency, not just the ones the glibc side-install actually satisfies. A
package genuinely missing a real dependency would silently "install"
broken.

**Real fix (not yet built):** register a synthetic `libc6` (and friends —
`libgcc-s1`, `libstdc++6`, whatever `$PREFIX/glibc`'s own `dpkg -l`
equivalent already tracks) into `$ADMINDIR/status` as `Provides:`/already
"installed," so real dependency resolution works normally and only
genuinely-missing dependencies fail. This is the "two-layer db" idea from
`design-install-path.md`, now with a concrete list of what to seed instead
of the earlier vague "don't seed at all" conclusion — that conclusion was
also based on the wrong "no glibc at all" assumption and needs revisiting:
seeding from `$PREFIX/glibc`'s own package database (it has one — it's
installed via Termux's normal `dpkg`) is likely exactly the right move
now, mirroring sudo-less's original design almost exactly, just seeding
from `$PREFIX/glibc` instead of `/`.

## Revised open work (replaces the equivalent items in other docs)

- [x] ~~Test `dpkg --instdir` + `--force-script-chrootless` against one
      real, simple glibc arm64 `.deb`~~ — done, works.
- [ ] Seed `$ADMINDIR/status` from `$PREFIX/glibc`'s own dpkg database
      (query it with `dpkg --admindir=$PREFIX/../var/lib/dpkg` or wherever
      Termux's glibc packages register themselves — not yet located) so
      `--force-depends` can be dropped for the common case.
- [ ] Resolve the `arm64`/`aarch64` architecture-name mismatch properly
      instead of `--force-architecture` globally — research whether dpkg
      supports any admindir-local architecture override.
- [ ] Re-test with a package that *has* maintainer scripts (`preinst`/
      `postinst`) — `hello` has none, so `--force-script-chrootless`'s
      actual behavior (scripts running with Termux's `/bin/sh`, seeing
      real absolute paths) is still unverified in practice.
- [ ] Re-test with a package with real library dependencies beyond libc
      (something linking `libssl`, say) to verify `grun --configure`'s
      `--findlib`/rpath behavior finds them under `$PREFIX/glibc`.
- [ ] Automate the manual steps above into `apt`'s `DPkg::Post-Invoke` hook
      per `design-hooks.md`, once seeding replaces `--force-depends`.
