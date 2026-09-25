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

## Update, same session: the static rewrite was the wrong tool entirely

The `update-alternatives` double-prefix above kept recurring even after
exempting it by name, traced to the same root cause hitting `base-files`
too: its postinst is **already `$DPKG_ROOT`-aware** (`"$DPKG_ROOT$1"`
throughout — a real, standard Debian convention for exactly this
chrootless-install scenario, more scripts follow it than just the
dpkg-suite tools). Naming individual tools to exempt doesn't scale.

**Fix: removed the static text path-rewrite entirely.** Kept only the
shebang rewrite to the `dn-dash` wrapper. This works because the runtime
`LD_PRELOAD` shim already covers exactly the complementary case — a
script with *no* `$DPKG_ROOT` awareness, using a bare literal path
(openssl's `ln -s /etc/ssl /usr/lib/ssl`) — by intercepting the actual
`symlink()`/`open()`/`exec()` call with the literal, unmodified path, not
by pre-editing the script's text. A `$DPKG_ROOT`-aware script's own
already-correctly-prefixed path never matches the shim's rewrite (it no
longer starts with a bare `/usr`, `/etc`, `/var`, `/opt`), so nothing
double-applies from that side either. Removing sed entirely also
eliminated the whole idempotency-marker mechanism (`# deb-native:
patched`) — no longer needed, since there's no more double-application
risk, and the shebang rewrite alone is naturally idempotent.

## The actual highest-value bug of the day: a sed delimiter mistake

Full writeup: `docs/findings-sed-delimiter-bug-2026-09-25.md`. Short
version: a `sed -E 's#^#!...#\2#'` call (extracting a shebang's trailing
flag, e.g. `-e`) used `#` as its delimiter while the pattern itself starts
with a literal `#` (matching `#!`) — `sed` doesn't parse this
semantically, so the first `#` after `^` closed the pattern section
immediately, producing `sed: unknown option to 's'`. Under this script's
`set -eu`, that single failure **silently aborted the entire per-file
loop**, every time, for every file alphabetically after whichever one
first hit it — which is exactly why `openssl.postinst` (sorts after
`base-files`, `base-passwd`, `dash`, `debconf`, `debianutils`) looked
permanently unfixable across many rounds of testing, when the actual
mechanism was fine. Fixed by switching to `,` as the delimiter. Found only
by invoking the script directly by hand and reading its own exit code —
not by reading the code harder, and not visible at all through the full
pipeline's logs, since a `|| true` one level up swallowed the crash.

## Where the bootstrap stands now

Fixed by the sed-delimiter fix alone: `openssl`, `dash`, `debianutils`,
`mawk` all reach fully configured (`ii`).

Two new, distinct, real findings — neither a bug in this project's
mechanism, both genuinely needing new work:

- **`chown` permission errors** (`base-passwd` on `/etc/subuid`,
  `base-files` on `/mnt`): a maintainer script's own explicit `chown`
  call, which this unprivileged process cannot satisfy regardless of what
  path it targets. No path redirect fixes a permissions problem. This is
  the first genuine case for sudo-less's actual "shim" concept (a fake,
  no-op stand-in command placed on `PATH`) — not path rewriting at all.
- **External commands the script forks don't get the shim**
  (`readline-common`'s postinst: `cp: cannot stat
  '/usr/share/readline/inputrc'`). The `dn-dash` wrapper deliberately
  `unset`s `LD_PRELOAD` right after starting, specifically so a forked
  Bionic binary (`cp`, found via `PATH`, likely Termux's own) doesn't
  crash trying to load a glibc `.so`. But that means `cp`'s own file
  access is **not** covered by the shim at all — a structural gap between
  "dash's own calls" (covered) and "anything dash forks as a separate
  process" (not, unless that process is itself a glibc binary this
  project also patches and separately arranges to run under the shim).

**Stopping here for this session** (quota-conscious, repeated direct
instruction). Concrete next steps, in order:
1. A shimmed no-op `chown` on the wrapper's `PATH` (audit for other
   root-only commands too: `chgrp`, real `mount`, etc.).
2. Decide how external commands a script forks should get shim coverage
   — install a real glibc `coreutils` via this project's own pipeline and
   arrange for the wrapper's `PATH` to prefer it (with the shim, since
   it'd be a glibc binary launched fresh, not a fork inheriting a cleared
   `LD_PRELOAD`)? Or accept this class of failure and treat it the way
   `chown` is being treated — case by case?
