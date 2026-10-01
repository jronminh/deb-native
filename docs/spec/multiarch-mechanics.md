# dpkg multi-arch mechanics: what they are, and what they aren't

Reference doc, shared across both the classic (separate-prefix) design and
the true fusion branch (`naibed`, formerly `fusion-no-prefix`) — the mechanics here are dpkg's own, and
apply identically regardless of which install strategy is on top of them.
Written after a real, live incident on the fusion branch (`docs/
findings.md` there, "Cross-check: a second, genuinely different package")
raised the question of whether some architecture-aware mechanism should
have prevented it. Researched against Debian's actual policy and dpkg's
own man pages rather than assumed — the answer clarifies what each
mechanism is actually *for*, which turns out to matter for how the two
designs use them differently.

## The `Multi-Arch` control field

Quoting Debian Policy directly: "A Debian installation can combine
packages from multiple architectures. The `Multi-Arch` field enables
individual packages to declare their support for this feature, and
influences the way **dependencies** are handled."

- `Multi-Arch: no` (default) — a `Depends`/`Provides`/etc. relation is
  satisfied only by a package of the *same* architecture.
- `Multi-Arch: same` — packages with the exact same name/version but
  different architectures can be installed **concurrently**; their files
  must be identical or architecture-specific (typically under
  `/usr/lib/${DEB_HOST_MULTIARCH}`). This project's own `native-seed.sh`
  stub packages use this field.
- `Multi-Arch: foreign` — this package's own architecture is irrelevant to
  whoever depends on it: it can satisfy a `Depends` from a package of
  *any* architecture. (Real Debian's `mawk` carries this field, alongside
  `Provides: awk` — confirmed directly from its own control file.)
- `Multi-Arch: allowed` — like `no`, but a dependent package may opt in to
  treating it as `foreign` by writing `pkg:any`.

All four are about **dpkg's dependency graph and on-disk library
coexistence**. None of them are about which file ends up at a given PATH
when two providers of the same command exist — confirmed against both
Debian Policy's control-fields chapter and the Debian wiki's
Multiarch/Hints page, which explicitly calls out file conflicts under
`Multi-Arch: same` as something *outside* the mechanism (the guidance is
"redesign the package," not "Multi-Arch resolves it").

## The two mechanisms that actually govern *paths*

- **`update-alternatives`** — real Debian's actual, and only, answer to
  "multiple providers can install the same command name; pick one by
  policy." It is a name → path → priority table with an auto/manual mode
  flag, and it is **completely architecture-agnostic by design** — it has
  no concept of which architecture registered an entry.
- **`dpkg-divert`** — lets an admin or a package's maintainer scripts move
  a specific path out of the way *before* a package's own unpack step
  would otherwise overwrite it — including a path dpkg doesn't own at all
  (a locally-modified or non-package file; its `--local` option is for
  exactly that, per its own man page). It does not cover a path created at
  *postinst* time by `update-alternatives` itself, since that path was
  never part of any package's own shipped file list to begin with.

## `--force-architecture` vs `--add-architecture`: not the same thing

Two different, easily-conflated dpkg mechanisms, verified by reading both
branches' actual scripts rather than assumed identical:

- **`dpkg --add-architecture ARCH`** is the *proper* mechanism: it adds
  `ARCH` as a real, tracked **foreign** architecture in dpkg's own
  database (`$ADMINDIR/arch` — one architecture per line, native first),
  alongside whatever dpkg already considers its own **native**
  architecture. Dependency resolution then correctly understands which
  declared architectures are legitimately installable side by side.
- **`--force-architecture`** is a blunt override: it tells dpkg to
  proceed *regardless* of an architecture mismatch, without registering
  anything. It doesn't add tracking — it just disables the check.

Confirmed by grep across the classic branch's own scripts
(`setup-apt-prefix.sh`, `bootstrap-base.sh`): it uses **only**
`--force-architecture`, never `--add-architecture`, and its own
`native-seed.sh` stub packages are written as `Architecture: arm64` (not
Termux's own native label) — i.e. the classic design's fresh, isolated
sandbox database is made to *pretend its native architecture is arm64
throughout*, real packages and local stubs alike, and `--force-
architecture` exists purely to get a dpkg binary whose own compiled-in
identity disagrees to stop objecting. This works because that database
starts completely empty and is *only ever* going to hold arm64 content —
there's nothing else in it whose real identity would be lost by the
pretense.

`naibed`, by contrast, **must** use the proper mechanism: it
reuses Termux's own real, pre-populated dpkg database, which already has
thousands of packages under its own native identity. That identity is
worth confirming precisely, not assumed:

```
dpkg --print-architecture            # aarch64 -- Termux's own historical label
dpkg --print-foreign-architectures   # arm64   -- what this branch added
```

Same physical CPU/ISA as any real Debian `arm64` package — genuinely two
different *architecture label strings* from dpkg's own perspective, not a
real instruction-set difference (no emulation/translation layer is
involved anywhere in either branch; the CPU executes both natively). A
blunt `--force-architecture` here would blur that distinction across a
database that has to keep both identities straight for everything already
in it — `--add-architecture` is what lets `aarch64`-native and
`arm64`-foreign packages coexist correctly in the one, real, shared
database, which is exactly fusion mode's whole premise.

## Net: which branch needs which, and why

| | Multi-Arch field | update-alternatives | `--force-architecture` | `--add-architecture` |
|---|---|---|---|---|
| Classic (separate prefix) | used for stub `Provides` (`native-seed.sh`) | used normally, no cross-namespace issue possible | used, sufficient (fresh, single-arch db) | not needed |
| `naibed` | used identically | used, **plus** needs bridging Termux's own non-dpkg providers into it (see `docs/fusion-multiarch.md` on that branch) | used too (still needed for the same dpkg-vs-package arch checks) | **needed** — shares Termux's real, pre-populated, `aarch64`-native database |

Neither `Multi-Arch` nor `update-alternatives` differs between the two
designs — they're dpkg's own layer, unmodified either way. What differs is
*how much of dpkg's real multi-arch machinery each design's database
actually has to exercise*: a fresh, disposable, single-arch sandbox can
get away with pretending; a shared, permanent, already-native database
cannot.
