# Findings: a proper base-package bootstrap (2026-09-25)

Direct follow-up to a design correction mid-session: instead of chasing
`debconf`/`cdebconf` bugs package by package, install the set of packages
real Debian assumes is "always already there" (`Priority: required`/
`important` — `base-files`, `base-passwd`, `dash`, `debianutils`,
`debconf`, `cdebconf`, plus `openssl`/`ca-certificates`) as **one bootstrap
transaction**, via a new `scripts/bootstrap-base.sh`.

## Real bug found and fixed: batched unpack/configure breaks Pre-Depends

`apt-install.sh` used to `--unpack` every downloaded `.deb` in one call,
then `--configure -a` once. Real failure: `base-files` **`Pre-Depends: awk`**
— a stricter-than-`Depends` check that blocks even `--unpack` (not just
`--configure`) until the pre-dependency is fully *configured*. Batching
unpack (all at once) then configure (all at once, in `dpkg`'s own internal
order — not necessarily matching Pre-Depends chains across separately
unpacked packages) left `mawk` still "unpacked but not configured" when
`base-files` needed it configured already.

**Fix:** `apt-install.sh` now processes packages **one at a time, in the
order apt itself resolved to fetch them** (parsed from apt's own `Get:`
lines during `--download-only` — dependency order, since a `Depends`/
`Pre-Depends` is always fetched before what needs it): `--unpack` then
immediately `--configure` for each, before moving to the next. A final
`--configure -a` sweep still runs at the end for anything left needing a
second pass (ordering-only, not real failures).

(Caught one indexing bug while building this: apt's `Get:N URL suite arch
PACKAGE arch version [size]` line has the package name in the **5th**
space-separated field, not the 4th — an early version of the parser
grabbed `arch` itself for every line and matched nothing.)

## Real bug found and fixed: stale downloaded `.deb`s get re-unpacked

`apt-install.sh` lists `.deb`s to process via `find "$ARCHIVES" -name
'*.deb'` — apt keeps every downloaded `.deb` in that cache by default. A
**later**, unrelated `apt-install.sh` call would pick up dash's own
`.deb` from an **earlier** transaction and `--unpack` it again, silently
overwriting an already `grun`-patched binary (working ELF interpreter)
with the archive's pristine, unpatched copy — found by `dash` breaking
again (`ENOENT` executing it) after a later, unrelated install. Fixed:
`apt-install.sh` now `rm -f "$ARCHIVES"/*.deb` at the end of every run.

## Real bug found and fixed: double-patching corrupts already-rewritten paths

Two patch passes exist for a good reason (`patch-deb.sh` pre-unpack, for
`preinst`'s timing; `patch-maintainer-scripts.sh` post-unpack, as a safety
net for a script patched before the wrapper existed yet) — but nothing
stopped the *same* script from being sed-rewritten by **both**, in one
run, turning `$INSTDIR/usr/bin/mawk` into
`$INSTDIR/$INSTDIR/usr/bin/mawk`. Fixed with an idempotency marker
(`# deb-native: patched`) written after the first pass; the
post-unpack safety net's *path*-rewrite is now skipped once the marker is
present, while its *shebang*-rewrite stays unguarded on purpose (it's
naturally idempotent — it only ever matches a plain `/bin/sh`-style
shebang, never the wrapper path it rewrites to — and this pass exists
specifically to catch a script patched before the wrapper existed).

## Real finding, not yet fixed: `update-alternatives` is already `DPKG_ROOT`-aware

The double-prefix bug above was traced further and turned out **not** to
be only the two-pass issue — it persisted even after the idempotency fix.
Root cause: `update-alternatives` (confirmed by `apt-dpkg-port.md` from
sudo-less itself: *"`update-alternatives` honours `DPKG_ROOT`: the links
and its database land in the prefix with no patch"*) **already** resolves
paths relative to `$DPKG_ROOT` — which dpkg sets to this project's prefix
automatically. This project's own blanket sed rewrite of `/usr/bin/mawk`
→ `$INSTDIR/usr/bin/mawk` inside `mawk`'s postinst, *before*
`update-alternatives` ever runs, hands it an already-prefixed path — which
it then prefixes *again* with `$DPKG_ROOT`, producing
`$INSTDIR/$INSTDIR/usr/bin/mawk`.

**Not fixed yet.** The real fix needs the path-rewrite step to recognize
calls to `update-alternatives` (and likely `dpkg-divert`,
`dpkg-statoverride`, `dpkg-trigger` — the same `DPKG_ROOT`-aware tools
sudo-less's own docs name) and leave *their* arguments alone, rewriting
only paths reaching genuinely prefix-unaware commands (`ln`, `.`/source,
`cp`, `exec`).

## Where the bootstrap stands

Most of the base set now reaches `ii` (fully configured): `libnewt0.52`,
`libselinux1`, `libslang2`, `libpcre2-8-0`, `libdebian-installer4`,
`libdebconfclient0`, `libreadline8t64`, `libtextwrap1`, `readline-common`,
`openssl`. Still stuck: `mawk` (the `update-alternatives` double-prefix
above), `base-passwd` (a *different*, likely unfixable-by-redirect issue —
its postinst calls real `chown` on `/etc/subuid`, which this unprivileged
process cannot do regardless of what path it targets; sudo-less's own
`0002-no-chown.patch` covers dpkg's *own* internal chowns, not a
maintainer script's explicit `chown` command — would need a shimmed
no-op `chown` on `PATH`, sudo-less's actual "shim" concept, finally a real
case for it), `debianutils`/`dash` (blocked transitively on `mawk`),
`debconf`/`cdebconf`/`ca-certificates` (blocked transitively on `dash`,
plus `debconf`'s own still-unresolved `exec /usr/share/debconf/frontend`
case — the file exists in the prefix, so this needs its own trace, not
yet done).

**Stopping here for this session** (quota-conscious, repeated direct
instruction). Concrete next steps, in order:
1. Exempt `DPKG_ROOT`-aware tools (`update-alternatives`, `dpkg-divert`,
   `dpkg-statoverride`, `dpkg-trigger`) from the path-rewrite pass.
2. A shimmed no-op `chown` (and audit for other root-only commands a
   maintainer script might call) on the wrapper's `PATH` — sudo-less's own
   "shim" concept, genuinely needed here for the first time.
3. Trace why `debconf.postinst`'s `exec /usr/share/debconf/frontend`
   still isn't redirected even once the wrapper mechanism is confirmed
   active for this script (unlike the earlier `cdebconf` case, the target
   file demonstrably exists in the prefix this time).
