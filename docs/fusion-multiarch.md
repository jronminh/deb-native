# This branch's real collision: two packaging systems, one filesystem

General dpkg multi-arch mechanics (`Multi-Arch` field, `update-
alternatives`, `dpkg-divert`, `--force-architecture` vs.
`--add-architecture`) now live in
[`docs/multiarch-mechanics.md`](multiarch-mechanics.md) — shared with
`main`, since none of that layer differs between the two designs. This
doc is the fusion-specific part: why the collision below happened here and
nowhere else, and what to build next.

Prompted by a real, concrete incident (below): installing `mawk:arm64`
replaced Termux's own `$PREFIX/bin/awk -> gawk` symlink, which briefly broke
`awk` system-wide (including this project's own `normalize-symlinks.sh`,
whose `relpath()` is implemented in `awk`). The question was whether real
Debian has an "architecture-aware" mechanism that should have prevented
this. Short answer, worked out in `multiarch-mechanics.md`: **no** — real
Debian's own `update-alternatives` handling of `mawk`'s postinst
(`update-alternatives --install /usr/bin/awk awk /usr/bin/mawk 5 …`) is
normal, correct, unremarkable behavior; nothing about it is wrong or
fusion-specific. `dpkg-divert` doesn't apply either: `mawk` never ships a
`/usr/bin/awk` file in its own archive, only `/usr/bin/mawk` — the
colliding path is created entirely at postinst time by `update-
alternatives`, which has no concept of diversions at all. (Diversion is
the right tool for a *different* collision class this branch already
handles: a package's own **shipped** file landing on an existing path —
that's what `fuse-classify.sh`'s pre-flight refusal, `docs/findings.md`,
already catches, by refusing before dpkg ever unpacks.)

## So what is actually new here, specific to naibed

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
