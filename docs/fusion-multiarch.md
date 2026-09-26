# Multi-arch mechanics, and why none of them solve this branch's real collision

Prompted by a real, concrete incident (below): installing `mawk:arm64`
replaced Termux's own `$PREFIX/bin/awk -> gawk` symlink, which briefly broke
`awk` system-wide (including this project's own `normalize-symlinks.sh`,
whose `relpath()` is implemented in `awk`). The question was whether real
Debian has an "architecture-aware" mechanism that should have prevented
this. Researched against Debian's actual policy and manual pages (not
guessed) — short answer: **no**, and the reason why is worth writing down,
because it clarifies what fusion mode actually needs to build instead.

## What `Multi-Arch` actually is (and isn't)

Quoting Debian Policy directly: "A Debian installation can combine packages
from multiple architectures. The `Multi-Arch` field enables individual
packages to declare their support for this feature, and influences the way
**dependencies** are handled."

- `Multi-Arch: no` (the default) — a `Depends`/`Provides`/etc. relation is
  satisfied only by a package of the *same* architecture.
- `Multi-Arch: same` — packages with the exact same name/version but
  different architectures can be installed **concurrently** (the classic
  case: a library, coexisting via an arch-tripled path like
  `/usr/lib/aarch64-linux-gnu/`).
- `Multi-Arch: foreign` — this package's own architecture is irrelevant to
  *whoever depends on it*: it satisfies a `Depends: awk` from a package of
  any architecture. This is the field `mawk`'s own control file actually
  carries (confirmed: `dpkg-deb -e` on the real `.deb`), alongside
  `Provides: awk`.
- `Multi-Arch: allowed` — like `no`, but a dependent package may opt in to
  treating it like `foreign` by writing `pkg:any`.

All four are about **dpkg's dependency graph and library coexistence on
disk** (arch-tripled paths). None of them are about **which file ends up at
a given PATH when two providers of the same command exist** — that's a
completely different concern, and Multi-Arch has nothing to say about it
(confirmed against both Debian Policy's control-fields chapter and the
Debian wiki's Multiarch/Hints page: file conflicts under `Multi-Arch: same`
are explicitly called out as something *outside* the mechanism — the
guidance is "redesign the package" or drop back to `Multi-Arch: no`, not
"Multi-Arch resolves it for you").

## The two real mechanisms, and which one actually applies

- **`update-alternatives`** — real Debian's actual, and *only*, answer to
  "multiple providers can install the same command name; pick one by
  policy." It is **completely architecture-agnostic by design**: it is
  just a name → path → priority table with an auto/manual mode flag. It
  doesn't know or care what architecture registered an entry. This is
  exactly the mechanism this branch is already using correctly (`docs/
  findings.md`, "Bug 4" and the runtime-stage fix) — `mawk`'s postinst
  calling `update-alternatives --install /usr/bin/awk awk /usr/bin/mawk 5`
  is real Debian's normal, correct, unremarkable behavior. Nothing about
  it is fusion-mode-specific or wrong.
- **`dpkg-divert`** — the other real mechanism, checked directly against
  its man page: it lets an admin or a package's maintainer scripts move a
  *specific path* out of the way before a package's own unpack step would
  otherwise overwrite it, including a path dpkg doesn't own at all (a
  locally-modified or non-package file — the `--local` option is for
  exactly that). This **does not apply to today's incident**: `mawk`
  never ships a `/usr/bin/awk` file in its own archive (only
  `/usr/bin/mawk`); `/usr/bin/awk` is created entirely at postinst time,
  by `update-alternatives`, which has no concept of diversions at all.
  Diversion is the right tool for the *other* collision class this branch
  already handles differently: a package's own **shipped** file landing on
  an existing path — that's what `fuse-classify.sh`'s pre-flight refusal
  (`docs/findings.md`) already catches, by refusing before dpkg ever
  unpacks, rather than diverting.

## So what is actually new here, specific to fusion-no-prefix

On a real Debian system, this scenario barely comes up: `gawk` would
*also* be a dpkg-managed package, registered into the *same* `awk`
alternatives group from its own postinst, at its own priority — two
providers competing inside one system that both understand the rules.

Fusion mode's actual, genuine novelty is bridging **two independent
packaging systems that have never heard of each other**: Termux's own
`pkg`/`apt` (which installed `gawk` and pointed `$PREFIX/bin/awk` at it
directly, as a plain symlink, with **zero** `update-alternatives`
involvement — Termux's own packaging doesn't use the alternatives system
for this at all) and Termux's *real*, native `dpkg` now also being used,
via this branch, to manage foreign-arch Debian packages in the very same
`$PREFIX`. Neither ecosystem's own tooling was ever asked to coordinate
with the other, because on every system either of them was designed for,
there was only ever one. This is not a bug in Multi-Arch, `update-
alternatives`, or Termux's own packaging — it's a genuinely new
integration surface this branch creates by design (installing into
Termux's real, live, shared root instead of a separate tree), and it's
this branch's own responsibility to build the missing bridge, not
something to find "the" existing mechanism for.

## The incident, concretely (2026-09-26)

1. `dpkg --configure mawk` ran `update-alternatives --install /usr/bin/awk
   awk /usr/bin/mawk 5 …` (via the wrapper this branch already built —
   working exactly as intended) and it selected `mawk` in auto mode, since
   it was the only registered alternative. This silently replaced
   Termux's plain `awk -> gawk` symlink.
2. Before the post-install `normalize-symlinks.sh` pass had run (the step
   that fixes the resulting portable-absolute targets — `docs/
   findings.md`, "Bug 4"), `$PREFIX/bin/awk` was a dangling symlink for a
   short window. `normalize-symlinks.sh`'s own `relpath()` helper shells
   out to `awk` — so the very tool meant to fix this broke on its own
   dependency, mid-run, surfacing as `awk: inaccessible or not found`
   part-way through, with zero symlinks fixed yet.
3. Manual recovery introduced a **second**, unrelated bug: hand-computing
   a relative symlink target by counting directory levels wrong
   (`etc/alternatives/awk -> ../bin/mawk`, missing one `..` — the real
   target needed `../../bin/mawk`, since `etc/alternatives/` is two levels
   below `$PREFIX`, not one). `readlink -f` and `stat -L` both correctly
   reported nothing resolved; `strace` on the failing `stat -L` pinned it
   to one `newfstatat(..., flags=0)` returning `ENOENT` for the whole
   chain, which is exactly what a wrong intermediate hop looks like. This
   is precisely why `normalize-symlinks.sh` computes `relpath()`
   programmatically instead of by hand — confirmed the hard way, not
   hypothetically.
4. Fixed properly by restoring `awk` first (by hand, carefully, to unblock
   the tool that needed it), then re-running the *real*
   `normalize-symlinks.sh` pass (now that its own `awk` dependency worked
   again), which correctly computed every link, including the deeper
   `share/man/man1/*.1.gz` ones (`../../../etc/alternatives/…`, three
   levels) — the exact generalization (`man1` vs. `figlet`'s `man6`) this
   cross-check was run to probe.

## What to actually build (not yet done)

Register Termux's own existing providers as *bona fide* competing entries
in the *same* alternatives group, at a priority that keeps them winning by
default, instead of leaving them as unmanaged plain symlinks a fresh dpkg-
managed alternative can silently displace:

```sh
update-alternatives --install /usr/bin/awk awk "$PREFIX/bin/gawk" 10
```

(`10 > 5`, mawk's own priority, so `gawk` keeps winning in auto mode; both
stay selectable via `update-alternatives --config awk`.) This isn't
fusion-mode-specific plumbing — it's the exact same mechanism already
working correctly for Debian-native providers, just extended to also
cover Termux's own. The missing piece is *detecting* which of Termux's own
installed files are plausible alternatives-group members worth registering
this way before a colliding dpkg package's postinst runs — genuinely
unbuilt, and the next real step here.
