# Findings: a genuinely hard package (`ruby-adsf`), 2026-09-25

Chosen deliberately as a hard case: pulls in `ruby3.3`, `libruby3.3`,
`debconf`, `ca-certificates`, `openssl`, `rubygems-integration` — a real
multi-package dependency tree with several maintainer scripts, not a
single-file leaf tool like `ciso`/`figlet`.

## Real bug found and fixed along the way: apt's own dpkg invocation shape

`scripts/apt-install.sh` originally ran a single `apt-get install`, relying
on `DPkg::Pre-Invoke` to run `patch-maintainer-scripts.sh` between unpack
and configure (`design-hooks.md`'s original plan). It never fired at the
right time: **apt calls dpkg once, and dpkg itself unpacks and configures
every package internally, back to back, in one process** — there is no
apt-level hook boundary between those two phases for a single
`apt-get install` transaction. `openssl`'s postinst (`ln -s /etc/ssl
/usr/lib/ssl`) kept failing even with the patch script wired in, because
it never ran before configure.

**Fix:** `apt-install.sh` now uses apt only to resolve the dependency
graph and download `.deb`s (`--download-only`), then drives `dpkg` itself
in three explicit steps: `--unpack` all downloaded `.deb`s, run
`patch-maintainer-scripts.sh` across the whole admindir, then `--configure
-a`. Verified: `openssl`'s postinst now configures successfully (it didn't
before this fix, in the exact same transaction).

## The actual remaining wall: `debconf`/`cdebconf`

With the fix, `debconf`'s own postinst got further — it correctly
`.`-sources `usr/share/debconf/confmodule` at its **rewritten** path
now (`.../root/usr/share/debconf/confmodule`, proof the sed patch worked
for debconf too) — but fails one level deeper:

```
.../confmodule[33]: /usr/lib/cdebconf/debconf: inaccessible or not found
```

`confmodule` is not a maintainer *control* script — it's a regular file
the `debconf` package ships under `/usr/share/debconf/`, sourced *by*
other packages' maintainer scripts. `patch-maintainer-scripts.sh` only
rewrites files under `$ADMINDIR/info/*.postinst` etc.; it has no reason to
touch an ordinary installed file, so `confmodule`'s own internal
`/usr/lib/cdebconf/debconf` reference (the actual `cdebconf` backend
binary, a whole separate IPC-based question/template protocol) is
untouched and still points at a real absolute path nothing sits at.

This is not a quick fix. `cdebconf` is a genuinely complex subsystem —
question databases, multiple frontends, templates, priorities — and
sudo-less's own survey lists exactly this class ("a dependency's trigger
or `postinst` (`tex-common`, `libreoffice-common`, `libgdiplus`)") under
"to study one by one," not something solved by a general mechanism.
Consistent with that: not chasing this further today. Fixing it properly
would mean generalizing `patch-maintainer-scripts.sh`'s rewrite to *any*
shell file a package installs that another script might source (a much
bigger surface — effectively "rewrite every shipped `.sh`/library file,"
not just control scripts), or accepting `debconf`-dependent packages as a
known excluded class for now, the way sudo-less accepts service/system-user
packages as out of scope.

## What this means for "is install work done"

Good news: the actual dependency-cascade in this transaction has real,
fixable root causes at every level checked so far (`openssl` — fixed;
`libruby3.3` — blocked only on ordering/a missing leaf, not investigated
further; `debconf` — a real, known-hard subsystem, not a bug in this
project's mechanism). Nothing here contradicts the two prior survey
results (2/30 → 10/30) — if anything it explains *why* `depmissing`/`script`
failures cluster the way they do: some are genuinely one-line fixes
(architecture name, epoch, this apt-invocation-shape bug), others
(`debconf`, likely `systemd`-integrated packages) are deep subsystems that
need their own dedicated design work, not a general "path redirect" fix.

**Not attempting a strict finish line for "install" here** — the sensible
scope, given today's fixes, is "single-package leaf tools and their real
dependency chains that don't route through `debconf`/`cdebconf`" — which
already covers a meaningful, tested slice (`hello`, `ciso`, `figlet`,
`libbs2b0`+its real deps, `openssl` itself). Moving on to runtime concerns
per today's plan; `debconf` stays a named, understood, open gap rather
than a silently ignored one.
