# Findings

The engineering log for the prototype (2026-09-25), merged and in
chronological order. (Merged from the former `findings-*.md`.)

## Findings: first working prototype (2026-09-25)


Tested by hand on-device against a real package from `deb.debian.org`
(`hello_2.10-5_arm64.deb`, no maintainer scripts — chosen deliberately as
the simplest possible case). Script: `scripts/prototype-install.sh`.

### Result: it works, end to end

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

### Major finding: Termux already ships a curated glibc side-install

Not previously known when `design.md` and
`design.md` were written (both assumed Termux "has no
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
  (`design.md`'s table, row "glibc-runner") is already
  solved, by Termux itself.** Nothing to build there beyond calling `grun
  --configure` on newly-installed ELF binaries — which is what
  `prototype-install.sh` does.

Consequence: this project's actual job shrinks to "get a real Debian
`.deb`'s files unpacked into a place `grun`-patched binaries can find their
libraries," not "build a whole path-virtualization system." Re-scope
`design.md`'s remaining open problem (hardcoded
`/etc`/`/usr`-internal paths with no env var) — that part is still real and
still unsolved, but it's a much smaller remaining surface than assumed.

### Two real blockers found, both worked around, neither properly solved yet

#### 1. Architecture name mismatch: `arm64` vs `aarch64`

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

#### 2. Empty admindir has no dependency chain registered

```
dpkg: dependency problems prevent configuration of hello:arm64:
 hello:arm64 depends on libc6 (>= 2.38).
```

A fresh, separate `$ADMINDIR` (deliberately kept apart from Termux's own,
per `design.md`) starts with zero packages registered, so
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
`design.md`, now with a concrete list of what to seed instead
of the earlier vague "don't seed at all" conclusion — that conclusion was
also based on the wrong "no glibc at all" assumption and needs revisiting:
seeding from `$PREFIX/glibc`'s own package database (it has one — it's
installed via Termux's normal `dpkg`) is likely exactly the right move
now, mirroring sudo-less's original design almost exactly, just seeding
from `$PREFIX/glibc` instead of `/`.

### Revised open work (replaces the equivalent items in other docs)

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
      per `design.md`, once seeding replaces `--force-depends`.
## Findings: a proper base-package bootstrap (2026-09-25)


Direct follow-up to a design correction mid-session: instead of chasing
`debconf`/`cdebconf` bugs package by package, install the set of packages
real Debian assumes is "always already there" (`Priority: required`/
`important` — `base-files`, `base-passwd`, `dash`, `debianutils`,
`debconf`, `cdebconf`, plus `openssl`/`ca-certificates`) as **one bootstrap
transaction**, via a new `scripts/bootstrap-base.sh`.

### Real bug found and fixed: batched unpack/configure breaks Pre-Depends

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

### Real bug found and fixed: stale downloaded `.deb`s get re-unpacked

`apt-install.sh` lists `.deb`s to process via `find "$ARCHIVES" -name
'*.deb'` — apt keeps every downloaded `.deb` in that cache by default. A
**later**, unrelated `apt-install.sh` call would pick up dash's own
`.deb` from an **earlier** transaction and `--unpack` it again, silently
overwriting an already `grun`-patched binary (working ELF interpreter)
with the archive's pristine, unpatched copy — found by `dash` breaking
again (`ENOENT` executing it) after a later, unrelated install. Fixed:
`apt-install.sh` now `rm -f "$ARCHIVES"/*.deb` at the end of every run.

### Real bug found and fixed: double-patching corrupts already-rewritten paths

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

### Real finding, not yet fixed: `update-alternatives` is already `DPKG_ROOT`-aware

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

### Where the bootstrap stands

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

### Update, same session: the static rewrite was the wrong tool entirely

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

### The actual highest-value bug of the day: a sed delimiter mistake

Full writeup: `docs/findings.md`. Short
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

### Where the bootstrap stands now

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
## Findings: a real glibc dash as the maintainer-script "shell" (2026-09-25)


Follows directly from a user insight during design discussion: dpkg was
confirmed (by strace) to just `execve()` a maintainer script directly,
letting the *kernel* resolve its `#!/bin/sh` shebang — dpkg itself never
picks which shell runs. So instead of trying to patch dpkg, or intercept
the *real* `/bin/sh` (root-owned `/system/bin/sh` on Android, confirmed
unpatchable — `design.md`), the shebang can simply be
rewritten to point at a shell this project fully controls: **a real
Debian `dash`, installed through this project's own `apt-install.sh`
pipeline, ELF-patched with `grun --configure` like any other glibc
binary** — closing the loop with infrastructure this repo already had,
rather than building a new Bionic-side mechanism.

### What's built and confirmed working in isolation

- `native/path-redirect.c` generalized from one hardcoded `FROM`/`TO`
  pair to a wholesale `/usr`, `/etc`, `/var`, `/opt` → `$DN_INSTDIR`
  mapping (same four directories sudo-less's view overlaid), plus
  `open64`/`stat64`/`lstat64`/`access`/`execv`/`execve` interceptors
  added alongside the original `open`/`openat`/`stat`/`fopen`/`symlink`.
- **Real Debian `dash`** (glibc, installed via `apt-install.sh dash`,
  patched via `grun --configure`) confirmed to need the `open64`/`stat64`
  family specifically — found via `readelf --dyn-syms`, which showed
  `dash` imports `open64@GLIBC_2.17`/`stat64@GLIBC_2.33`/etc., not the
  plain names the shim first only covered. Fixed and verified: `dash -c
  '. /etc/foo.conf'` correctly sources the redirected file and prints its
  content.
- `patch-maintainer-scripts.sh` now also generates a wrapper
  (`$INSTDIR/usr/bin/dn-dash`) and points each control script's shebang
  at it instead of `/bin/sh`.

### Two real bugs found and fixed while wiring this into the real pipeline

1. **`LD_PRELOAD` cannot be exported before calling `dpkg` itself.**
   `dpkg` is a Bionic process; Bionic's linker refuses to even start with
   a glibc `.so` in `LD_PRELOAD` (`CANNOT LINK EXECUTABLE ... library
   libc.so.6 not found`) — confirmed directly (`dpkg --version` under
   that env crashes outright). The wrapper script sets `LD_PRELOAD` only
   for the `dash` process it execs, never touching `dpkg`'s own
   environment.
2. **`LD_PRELOAD` then leaked into `dash`'s own children.** A maintainer
   script forking `cp` (a Bionic binary, found via `PATH`) crashed the
   same way, since environment variables propagate to forked children by
   default. Fixed by inserting `unset LD_PRELOAD 2>/dev/null || true` as
   the script's own second line (right after its rewritten shebang) —
   `dash`'s own interposition, already resolved at its process-load time,
   keeps working for the rest of its life regardless of the env var being
   unset afterward; only children forked *after* that point stop
   inheriting it.
3. **`PATH` needed the prefix's own `bin`/`sbin` first.** `debianutils`'s
   postinst calls `update-shells` by bare name; the wrapper now exports
   `PATH="$INSTDIR/usr/sbin:$INSTDIR/usr/bin:...:$PATH"`.
4. **Missing base skeleton directories** (`var/lib`, `var/log`,
   `var/cache`) — `setup-apt-prefix.sh` now precreates them; a package's
   own postinst assuming they exist (as they would on real Debian, seeded
   by `base-files`, which this project never installs) failed without
   them.

### Where it stands: real progress, not a finished mechanism

With all of the above, `openssl`'s postinst (`ln -s /etc/ssl /usr/lib/ssl`)
and a plain `. /etc/foo.conf` test both configure/run correctly through
the full real pipeline (`setup-apt-prefix.sh` → `apt-install.sh`).

`debconf`'s deeper case (the actual motivating hard problem —
`confmodule` calling `exec /usr/lib/cdebconf/debconf`) got **further than
before** — the error changed from `. /usr/share/debconf/confmodule: No
such file or directory` (couldn't even find confmodule) to `exec:
/usr/lib/cdebconf/debconf: not found` (found confmodule via the redirect,
reached the `exec`, but the exec target itself wasn't redirected) — but
is **not yet fully working**. Working hypothesis, not yet verified:
`dash`'s `exec` builtin likely checks the target's existence via
`access()`/`faccessat()` *before* calling the real `execve()`, and (same
pattern as `open`→`open64`) the actual symbol dash's `exec` implementation
calls might not be the plain `access` this shim already intercepts —
needs the same `readelf --dyn-syms` treatment `open`/`stat` already got.

### Follow-up, same session: two more real fixes, then a bigger wall

- **`faccessat`, not `access`.** `readelf --dyn-syms` on `dash` confirmed
  it: `dash`'s own `exec` builtin checks the target via `faccessat`
  (`UND faccessat@GLIBC_2.17`), not the plain `access` this shim already
  intercepted. Added.
- **`cdebconf` is a separate package `debconf` doesn't strictly `Depends:`
  on.** `apt-cache show debconf` has no hard dependency on it at all —
  real Debian assumes it's already present (it's `Priority: important`,
  part of the base install apt normally never has to think about). Our
  from-scratch prefix has nothing "already there," so it must be
  installed explicitly. `apt-cache show cdebconf` confirms it `Provides:
  debconf-2.0`, satisfying `ca-certificates`'s alternative dependency
  directly.
- Also added `DEBIAN_FRONTEND=noninteractive` to the wrapper (standard
  Debian practice for unattended installs, prevents a `debconf` prompt
  from hanging) and a second `--configure -a` pass in `apt-install.sh`
  (resolves ordering-only failures like `libruby3.3 depends on
  ruby-ruby2-keywords` after the package that provides it configures).

**New, deeper wall found installing `cdebconf` itself:**

```
mkdir: cannot create directory '/var': Read-only file system
dpkg: ... new cdebconf:arm64 package pre-installation script subprocess returned error exit status 1
```

`cdebconf`'s `preinst` (`cdebconfdir="/var/lib/cdebconf"; mkdir -p
$cdebconfdir`) hits the same hardcoded-path class of problem — but this
one is architecturally different from every case fixed so far. Checked
directly: the sed pattern *would* rewrite this fine (`="` followed by
`/var/` matches; the earlier worry that a variable-held path would be
invisible to sed was wrong for this actual case). The real problem is
**timing**: a package's `preinst` runs as part of `dpkg`'s `--unpack` step
itself, *before* `patch-maintainer-scripts.sh` ever gets to run (it runs
*after* `--unpack` in this pipeline) — so `preinst` scripts are never
patched at all, for any package, regardless of what they contain.

Fixing this needs a real architecture change, not a tweak: patch a
package's control scripts (and rewrite the shebang) **inside the `.deb`
file itself** — extract, patch, repackage — before ever handing it to
`dpkg`, instead of patching `$ADMINDIR/info/*` after the fact. That's a
meaningfully bigger piece of work (handling `md5sums` consistency, a
`dpkg-deb --build` round-trip per package) than anything else in this
doc. **Stopping here for this session** (quota-conscious): the debconf/
`cdebconf` chain is understood in real depth now, but not yet fully
closed. Next concrete step, in order: (1) build the pre-unpack `.deb`
patching pipeline, (2) retest `cdebconf` → `ca-certificates` →
`ruby-adsf` end to end.
## Findings: a genuinely hard package (`ruby-adsf`), 2026-09-25


Chosen deliberately as a hard case: pulls in `ruby3.3`, `libruby3.3`,
`debconf`, `ca-certificates`, `openssl`, `rubygems-integration` — a real
multi-package dependency tree with several maintainer scripts, not a
single-file leaf tool like `ciso`/`figlet`.

### Real bug found and fixed along the way: apt's own dpkg invocation shape

`scripts/apt-install.sh` originally ran a single `apt-get install`, relying
on `DPkg::Pre-Invoke` to run `patch-maintainer-scripts.sh` between unpack
and configure (`design.md`'s original plan). It never fired at the
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

### The actual remaining wall: `debconf`/`cdebconf`

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

### What this means for "is install work done"

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
## Findings: a real sed-delimiter bug that silently broke everything (2026-09-25)


The single highest-value bug found today, by far: not a design flaw, a
plain shell scripting mistake that made the whole shebang/wrapper
mechanism look broken for hours of testing.

### The bug

```sh
flag=$(printf '%s' "$shebang" | sed -E 's#^#![[:space:]]*/bin/...#\2#')
```

Using `#` as the `sed` delimiter, in a pattern that itself starts with a
**literal `#`** (matching a shebang's own `#!`). `sed` parses the
delimiter positionally, not semantically — the very first `#` after `s`
closes the "pattern" section immediately (as `^`, an empty-ish pattern),
and the real pattern text (`!...`) gets read as something sed can't
parse as a valid trailing section, producing:

```
sed: -e expression #1, char 78: unknown option to `s'
```

Under this script's `set -eu`, this single failing `sed` call **aborted
the entire script**, mid-loop, for whichever maintainer script happened
to be processed at that point (alphabetically before `openssl.postinst`,
in every run — `base-files`, `base-passwd`, `dash`, `debconf`,
`debianutils`, all sort earlier than `openssl`). Every file
alphabetically *after* the crash point silently never got touched, in
*every single test run* — indistinguishable, from the outside, from "the
shebang mechanism just doesn't work for this case," which is exactly what
several rounds of testing today concluded before this was found.

### How it was actually found

Not by reading the code harder. By directly invoking
`patch-maintainer-scripts.sh` by hand against an already-built test
prefix and looking at its own exit code and stderr — `sed: unknown option
to 's'`, `exit: 1` — instead of only ever running it embedded inside the
full multi-minute bootstrap pipeline (where a `|| true` at the call site
swallowed the failure silently, downstream errors in the log looked like
independent, unrelated bugs, and there was no way to tell "this script
crashed" from "this script ran and decided not to do anything here").

### The fix

Use a delimiter that appears in neither the pattern nor the replacement —
`,` here (`|` was tried first and rejected: it's the regex alternation
operator inside `(sh|bash|dash)`, so it would have broken delimiter
parsing the same way, just for a different reason):

```sh
flag=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/...,\2,')
```

### What this unblocked

With this one fix, `openssl` went from permanently stuck (`iF`, `ln:
failed... /usr/lib/ssl`) to fully configuring correctly — its shebang
finally got rewritten to the `dn-dash` wrapper (with its own `-e` flag
correctly preserved, a separate fix made earlier the same session:
`grep -qE '^#!\s*/bin/(sh|bash|dash)\s*$'` also never matched
`#!/bin/sh -e` at all, since `\s` is PCRE, not POSIX ERE, and the pattern
required nothing after the interpreter name). `dash`, `debianutils`, and
`mawk` also reached fully configured (`ii`) as a direct result, since
their own control scripts were among the earlier casualties of the same
crash.

### Lesson for the rest of this project

**Test the actual failing tool in isolation before concluding a mechanism
doesn't work**, especially for anything wrapped in `set -eu` plus a
swallowing `|| true` one level up — the combination is specifically
dangerous because it converts "this script crashed outright" into
silence, indistinguishable from "this script ran and correctly decided
nothing needed doing." Both `patch-deb.sh` and
`patch-maintainer-scripts.sh` had the identical bug (copy-pasted between
them) and both needed the identical fix.
## Findings: a complete base bootstrap, and the runtime fix that unblocked it (2026-09-25, PM)


Outcome: on a **fresh** prefix, `scripts/setup-apt-prefix.sh` +
`scripts/bootstrap-base.sh` now bring the entire base set to
`Status: install ok installed` (`ii`) — `base-files`, `base-passwd`,
`dash`, `debianutils`, `debconf`, `cdebconf`, `openssl`,
`ca-certificates`, `mawk`, and their real dependency libraries (28
packages). Confirmed by `dpkg --admindir=.../var/lib/dpkg -l` and by a
functional check inside the prefix (`/etc/os-release` reports Debian 13
trixie; `dash`, `mawk`, `openssl` resolve to the prefix and run). Then
`scripts/apt-install.sh` installs a *new* package on top: `hello` lands and
prints `Hello, world!`.

This closes the two gaps
[`findings.md`](findings.md)
named as next steps (no-op `chown`; forked external commands need glibc
coverage) and the chicken-and-egg it left open for a fresh prefix.

### Root causes found and fixed (each by direct on-device testing)

1. **The maintainer-script interpreter was a shell script.** `dn-shell`
   was `#!/system/bin/sh` + settings. The kernel follows only **one** `#!`
   level, so `#!$INSTDIR/usr/bin/dn-shell` (where `dn-shell` is itself a
   script) is unexecutable; `dpkg` falls back to Android's Bionic
   `/bin/sh`, the glibc `LD_PRELOAD` never applies, and the run surfaces as
   the misleading
   `CANNOT LINK EXECUTABLE "/bin/sh": library "libc.so.6" not found`.
   **Fix:** `native/dn-launch.c` — a tiny **Bionic ELF** launcher (built
   with Termux's own `clang`) that derives `$INSTDIR` from
   `/proc/self/exe`, sets `LD_PRELOAD`/`DN_INSTDIR`/`PATH`/`DEBIAN_FRONTEND`,
   and `exec`s Termux's glibc `bash` (or, when invoked as `dn-perl`, glibc
   `perl`). `scripts/setup-runtime.sh` builds it and installs
   `dn-shell`/`dn-perl`.

2. **The chicken-and-egg (no Debian `dash` on a fresh prefix).** The old
   wrapper pointed at a Debian `dash` this project had to install first, so
   on a fresh bootstrap no wrapper could be created and `preinst` ran raw
   (cdebconf's `mkdir -p /var/lib/cdebconf` → `Read-only file system`).
   **Fix:** reuse Termux's **pre-existing** glibc userland
   (`$PREFIX/glibc/bin/bash` + `coreutils-glibc`), which exists before any
   Debian package is unpacked. This also makes commands a maintainer script
   *forks* glibc, so the shim reaches them — the gap the previous doc left
   open.

3. **`chdir` was not intercepted.** coreutils `mkdir -p` verifies an
   existing component with `chdir()`, not `stat()` (confirmed by strace:
   `mkdirat("$INSTDIR/var") = EEXIST` then `chdir("/var") = ENOENT`, the
   real `/var` being absent on Android), which coreutils reads as "not a
   directory". **Fix:** `chdir` interposer in `native/path-redirect.c`.
   Every `mkdir -p` on a path whose parent already exists was failing
   because of this.

4. **`grun --configure` rewrote this project's own runtime ELFs.**
   `apt-install.sh` grun-patches every ELF in the prefix; that corrupted
   the Bionic launcher and the shim (their interpreter became the glibc
   target, hence `libdl.so: cannot open shared object file`). **Fix:** the
   grun loop now skips `usr/bin/dn-shell`, `usr/bin/dn-perl`, and
   `usr/lib/deb-native/`.

5. **The shim's own home made `base-files` abort.** Placing it at
   `$INSTDIR/lib/deb-native` made `$DPKG_ROOT/lib` a directory, which
   `base-files`' `preinst` usrmerge check reads as "install usrmerge first"
   and refuses to unpack. **Fix:** moved the shim to
   `$INSTDIR/usr/lib/deb-native/`.

6. **`execvp` bypassed the shim.** glibc's `execvp`/`execvpe` walk `PATH`
   and call `__execve` *internally*, off the dynamic symbol table, so
   exporting `execve` alone never saw them. debconf's Perl frontend
   re-runs a package's config script with `execvp`; uncaught, the kernel
   resolved that script's shebang and handed Perl's glibc `LD_PRELOAD`
   straight to the Bionic interpreter. **Fix:** reimplemented
   `execvp`/`execvpe` (plus `execl`/`execlp`/`execle`) in the shim,
   funnelling through the same `do_exec` that strips `LD_PRELOAD` for
   non-glibc targets.

7. **Termux's `dpkg-realpath` is missing its data file.** It sources
   `$PREFIX/share/dpkg/sh/dpkg-error.sh`, which Termux's `dpkg` package
   does not ship. Pre-placed it (from Debian's `dpkg` package) at
   `$PREFIX/share/dpkg/sh/`; `dpkg-realpath` now runs clean.

Also added: no-op `chown`/`chgrp` shims on the wrapper `PATH`, and a much
wider intercept set in the shim (`statx`, `mkdirat`, `unlinkat`,
`symlinkat`, `linkat`, `renameat`/`renameat2`, `fchmodat`, `truncate`,
`utimensat`, `readlink`/`readlinkat`, `opendir`, `faccessat2`, `chdir`),
plus an `execve`/`execveat` dispatch that keeps `LD_PRELOAD` for glibc
targets and strips it for Bionic/scripts — which is what let the old
`unset LD_PRELOAD` hack be removed.

### Remaining, minor

- `dpkg-statoverride` is not shipped by Termux's `dpkg`; `ca-certificates`
  `postinst` logs `dpkg-statoverride: command not found` but still reaches
  `ii`. Either provide a no-op shim or pre-place the tool (it is a dpkg
  binary, so a shim is the lighter option).

### Files changed

- `native/dn-launch.c` (new), `native/path-redirect.c` (extended),
  `scripts/setup-runtime.sh` (new),
  `scripts/patch-deb.sh`, `scripts/patch-maintainer-scripts.sh`,
  `scripts/apt-install.sh`, `scripts/setup-apt-prefix.sh`.
## Findings: first random-sample survey (2026-09-25)


Following sudo-less's own methodology (`docs/survey-2026-09.md`,
`dev/survey.sh`): a random, reproducible sample of real Debian packages,
each installed into a fresh isolated prefix. `scripts/sample-packages.py`
(seed `20260925`, ≤2 per section, ≤5MB, excluding required/important/
standard-priority and metapackages) + `scripts/survey.sh`.

### Result: 2 of 30 installed (≈7%)

| install outcome | count |
|---|---|
| `ok` | 2 |
| `depmissing` | 27 |
| `unpack` | 1 |

Far below sudo-less's own 63% (`docs/survey-2026-09.md`), and far below
the "even 40% would be great" bar set going in. Both is honest data, not a
discouraging surprise once the cause is clear — see below.

### The real cause: this project has no dependency installer, at all

27 of 30 failures are `depmissing`: dpkg refuses to configure the package
because a dependency isn't present. This is **not** the native-seed
mapping gap `design.md` flagged as open work (a ~10-entry
hand-written table of Debian-name → Termux-`*-glibc`-package). That gap
would only explain failures on `libssl3`/`zlib1g`/etc. — a handful of the
27. The actual pattern, reading the failures:

```
libparse-bbcode-perl depends on libclass-accessor-perl; however: Package libclass-accessor-perl is not installed.
worker:arm64 depends on worker-data.  worker:arm64 depends on avfs (>= 1.2.0).
rocksdb-tools:arm64 depends on libgcc-s1 (>= 4.2).  depends on libgflags2.2 (>= 2.2.2).
golang-github-bep-tmc-dev depends on golang-github-frankban-quicktest-dev; ...
```

These are **ordinary Debian package dependencies** — other `.deb`s that
would need to be downloaded and installed too, exactly what `apt-get
install` does automatically by walking the dependency graph. **This
project has never actually done that.** `scripts/prototype-install.sh`
only ever unpacks the *one* `.deb` it's given, plus whatever
`native-seed.sh`'s small stub table covers. `design.md`
always intended real `apt` (not bare `dpkg`) for exactly this reason — but
that plan was never implemented or tested; every prototype and survey run
so far has used bare `dpkg` on a single file.

**This is the actual #1 priority now**, well ahead of anything else open
in this repo's docs (soname-based native mapping, generalizing the
`LD_PRELOAD` shim, etc.) — none of that matters if a package's own
ordinary dependencies were never fetched in the first place.

### The one `unpack` failure: not chased further

`libxmlezout-dev` failed on `Permission denied` unpacking a 35-character
filename — not a path-length issue (checked: well under any real limit).
Not investigated further; the prototype is still too early-stage for a
single odd failure like this to be worth a deep dive yet (could be the
survey's own very deeply-nested scratch `OUT` directory path, could be
something else — unknown).

### What this changes

- `design.md`'s "Open work" (soname mapping, unverified table
  entries) is still valid work, but it's downstream of a much bigger gap:
  without real dependency resolution, most real packages never get far
  enough to need it.
- The right next step is wiring actual `apt` against a real prefix-scoped
  `apt.conf` (as `design.md` originally described but this
  repo never actually built) so ordinary dependencies get fetched and
  installed automatically — `native-seed.sh`'s stub table then only needs
  to cover the specific case it was built for (a dependency Termux's
  glibc side-install already provides, which should NOT be re-fetched),
  not stand in for a missing installer entirely.
- Re-run this same survey (same seed, same script) once real `apt`
  dependency resolution exists, to get a real before/after comparison.

### Raw data

`scripts/survey.sh` + `scripts/sample-packages.py` are committed and
reproducible (same seed `20260925`) — re-running produces the same 30
packages. Full per-package log kept locally during this run
(`OUT/logs/*.log`), not committed (large, single-run artifact).
## Findings: wiring real apt — 2/30 → 10/30 (2026-09-25)


Direct follow-up to `findings.md`, which found the
dominant failure cause (27/30) was that this project never actually
installed a package's dependencies — `prototype-install.sh` only unpacked
the one `.deb` it was given. This round builds and tests the fix: real
`apt`, pointed at a real Debian repo, scoped to a separate prefix.

### The fix: `scripts/setup-apt-prefix.sh` + `scripts/apt-install.sh`

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

### Same 30-package sample, same seed, real result: 10/30 (33%)

| outcome | count |
|---|---|
| `ok` | 10 |
| `unresolvable` | 10 |
| `script` | 6 |
| `unpack` | 4 |

Up from 2/30 (7%) with bare `dpkg`. Real progress, and close to (though
short of) the "even 40% would be great" bar set going in — with two new,
previously-unseen failure categories now dominant instead of `depmissing`.

### New finding: our shim doesn't reach maintainer scripts

Two of the 6 `script` failures are the *same class* of problem
`design.md` already solved for compiled binaries —
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

### New finding: dpkg's intra-package hardlinks fail on this filesystem

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

### `unresolvable`: apt's solver, working as intended, hitting a real limit

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

### What changes because of it

- The install-path work (`design.md`) should be updated:
  `apt` is now real and tested, not just planned.
- `design.md` needs a scope correction: its `LD_PRELOAD`
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
## Findings: path-redirect shim performance (2026-09-25)


Goal: make the `LD_PRELOAD` shim not a measurable drag on installed
programs. `rewrite()` sits on the hot path of every intercepted
`open`/`stat`/`exec`/`mkdir`/... call, so it is the only place worth
optimizing.

### What changed

Before, every intercepted call did:

```
getenv("DN_INSTDIR")            # linear scan of the environment
for each of /usr /etc /var /opt: strlen(prefix); strncmp(path, prefix, len)
snprintf(buf, bufsz, "%s%s", root, path)   # on a match
getenv("DN_REDIRECT_DEBUG")     # on a match
```

Now (`native/path-redirect.c`):

- **Cached at load.** An `__attribute__((constructor))` reads
  `DN_INSTDIR`, its length, `DN_REDIRECT_DEBUG` and `DN_BIONIC_PRELOAD`
  once. The environment is fixed before `exec` by the launchers, so
  caching is safe; no `getenv()` runs per call.
- **Branch dispatch on `path[1]`.** `/usr`, `/etc`, `/var`, `/opt` are
  all four bytes long, so a `switch (path[1])` picks the candidate prefix
  in one compare; a path that is not one of the four (the common case:
  `$INSTDIR/...`, relative paths, `/dev`, `/proc`, `/system`) returns
  after that single compare instead of four `strlen`+`strncmp` rounds.
- **`memcpy` instead of `snprintf`** to build the rewritten path.
- `scripts/build-path-redirect.sh` now compiles with **`-O2`**; clang's
  default is `-O0`, so the shim was previously built unoptimized.

### What the numbers actually say

On-device microbenchmarks are dominated by things that are not the shim:
Android `stat` on a missing path and Termux `fork`/`exec` cost tens of
microseconds to milliseconds, while `rewrite()`'s contribution is tens to
hundreds of nanoseconds. Repeated runs of the same tight loop
(`for …; do [ -e /usr/share/x ]; done`) disagreed with each other by
large margins (an early interleaved run showed the old shim ~35 % slower
than no shim and the new one ~3–6 % better; a later run was within noise;
a 2 M-iteration run took 37 s with no shim at all, i.e. the loop, not the
shim, is the cost).

So no stable end-to-end percentage is claimed. What is solid and does not
depend on measurement:

- the fast-reject path is now one compare, where it used to be four
  `strlen`+`strncmp` rounds;
- a rewrite no longer calls `getenv`/`strlen`/`snprintf` per call;
- real programs mostly open `$INSTDIR/...` (fast reject) or relative
  paths (no rewrite); the shim only does extra work for a literal
  `/usr`, `/etc`, `/var`, `/opt` path, which is its whole purpose.

### Separately: `grun --configure` is free at run time

`grun --configure` rewrites a `.deb`'s ELF `PT_INTERP` to
`$PREFIX/glibc/lib/ld-linux-aarch64.so.1` and adds a `RUNPATH` of
`$PREFIX/glibc/lib`. Both are one-time static edits — no wrapper, no
`ptrace`, no trampoline, no emulation, so the patch adds nothing at run
time. The visible cost of running an installed program is Termux's
`fork`/`exec` latency, not the patch or the shim.


## Platform sandbox limits, by direct probe (2026-09-26)

Probed the device from both sandboxes: the Termux app uid (`untrusted_app_27`)
and the seccomp-free Android shell uid (`u:r:shell:s0`, reached with `dsh`,
which runs commands at uid 2000). Android 16 userspace, kernel 5.10.240, arm64.

| | Termux app uid | shell uid (`dsh`) |
|---|---|---|
| uid / SELinux | 10663, `untrusted_app_27` | 2000, `u:r:shell:s0` |
| CapEff | 0 | 0 |
| seccomp | filter active (`Seccomp: 2`, 1 filter) | **none** (`Seccomp: 0`) |
| `unshare(CLONE_NEWUSER)` | `EINVAL` | `EINVAL` |
| `unshare(CLONE_NEWNS)` | `EPERM` | `EPERM` |
| `/dev/fuse` | — | `crw------- root root` (unreadable) |
| read `$PREFIX` | yes (owner) | no (`/data/data/com.termux` permission denied) |
| `run-as com.termux` | — | `package not debuggable` |

Findings:

- **User namespaces are off kernel-wide, not an app restriction.**
  `unshare -U` returns `EINVAL` even from the seccomp-free shell, and
  `/proc/self/ns/` has no `user` entry — the kernel is built without
  `CONFIG_USER_NS`. This is the hard reason sudo-less's kernel "view" (user +
  mount ns + unprivileged overlayfs) cannot exist here: no sandbox or
  permission unlocks it.
- **Mount namespaces and overlayfs are unreachable.** `unshare(CLONE_NEWNS)`
  and `mount` need `CAP_SYS_ADMIN`; `CapEff` is `0` in both domains and
  SELinux is enforcing. The kernel does list `overlay` and `fuse` in
  `/proc/filesystems`, but overlay needs mount privileges and `/dev/fuse` is
  root-only — neither is usable.
- **`ptrace` works in the app domain.** `proot -0` runs in Termux (fake uid 0,
  context still `untrusted_app_27`), so a `ptrace`-based syscall tracer is
  feasible from the app uid. A tracer in the shell domain is not: cross-domain
  tracing would need `CAP_SYS_PTRACE` and access to the app prefix, both absent.
- **`dsh`/shell is a probe and provisioning tool only.** Its lack of a seccomp
  filter makes it useful for reading the kernel's real state, but it cannot
  host the enforcement layer — it can neither read the private prefix nor
  `run-as` the app.
- **The app already runs under one seccomp filter.** Any filter a project adds
  stacks on top (most restrictive wins); Android's baseline cannot be lifted.
  That baseline, plus SELinux, is why `CLONE_NEWUSER` is denied even where
  seccomp is off.

Net: the only mechanisms available are (1) libc-level interposition — the shim
this project ships — and (2) a syscall-level tracer via `ptrace` /
`SECCOMP_RET_USER_NOTIF` **inside the app uid**. Namespaces, overlayfs and
FUSE are off the table by kernel and SELinux policy, exactly as the design
assumed.

## Findings: finishing the libc-level shim (2026-09-26)

The code review in
[#1](https://github.com/jronminh/deb-native/issues/1) listed what the
libc-level shim could not yet see. `521cc73` closed the review items
(`unlink()` bug; `lstat`; the missing `*64` names; fortified `__open_2`/
`__openat_2`/`__open64_2`; `statfs`/`statvfs`; `dlopen`/`dlmopen`; AF_UNIX
`bind`/`connect`). This round finishes the layer, so the remaining gap is
only the one libc interposition inherently cannot cover (raw syscalls,
static binaries, libc-internal opens) — the syscall tracer's job.

### What was added

The rest of the path-taking libc surface: `creat`/`creat64`/`freopen`;
`chown`/`lchown`/`fchownat`; `utime`; the xattr family (`setxattr`/
`lsetxattr`/`getxattr`/`lgetxattr`/`listxattr`/`llistxattr`/`removexattr`/
`lremovexattr`, which is how dpkg and capability-aware tools touch files);
`mkfifo`/`mkfifoat`/`mknod`/`mknodat`; `statfs64`/`statvfs64`; `realpath`/
`canonicalize_file_name`; `inotify_add_watch`; AF_UNIX `sendto` (reusing the
existing `rewrite_sockaddr`); the temp-file templates `mkstemp`/`mkostemp`/
`mkdtemp`; and `posix_spawn`/`posix_spawnp`.

Two of these needed care beyond the usual rewrite-and-call:

- **`mkstemp`/`mkostemp`/`mkdtemp` modify the caller's template in place**,
  and that buffer is only `strlen(template)+1` long. Rewriting it to
  `$INSTDIR/etc/...` cannot be copied back. The shim calls the real function
  on the rewritten buffer, then copies back only the part after `$INSTDIR`
  (the random suffix included), after a length check against the caller's
  original template. Verified: the caller sees `/etc/zz_mkstemp9wnoFI` while
  the file is created under `$INSTDIR/etc/`.
- **`posix_spawn` bypasses the interposed `execve`** (glibc uses
  clone+exec internally), so it gets its own wrapper: rewrite the path, keep
  the environment for a glibc target and swap in `bionic_env()` otherwise.
  `posix_spawnp` walks `$PATH` itself (glibc's internal walk is invisible
  here) and delegates to `posix_spawn`. This matters because modern glibc
  and coreutils spawn helpers through `posix_spawn`, not `fork`+`execve`.

### How it was verified, on-device

`tests/shim-libc/run.sh` builds a standalone glibc test binary, sets up a
fake `$DN_INSTDIR` root, runs the test under the shim with
`DN_REDIRECT_DEBUG=1`, and asserts that every intercepted symbol rewrote its
path to the root and that nothing leaked into the real `/etc`.

Two on-device facts made this possible and are worth recording:

- **The glibc side-install does ship `Scrt1.o`/`crti.o`/`crtn.o`** (an
  earlier note said a standalone glibc executable could not be linked, but
  only because it was looking for `crtbeginS.o`/`crtendS.o`/`libgcc.a`).
  Linking with `-nostartfiles -nodefaultlibs` and naming those three objects
  explicitly produces a runnable glibc executable with the same clang that
  builds the shim.
- Tests that need a real child (`posix_spawn`) copy a glibc binary under
  `$INSTDIR/usr/bin/` and spawn the `/usr/bin/...` path; the child runs and
  exits 0, proving the redirect reached the real spawn, not just this
  process's own libc calls.

Result: 33 rewrites asserted, all new symbols covered, `mkstemp`/`mkdtemp`
templates handed back un-prefixed, and the redirected `posix_spawn` child
exits 0. Real `/etc` is untouched. The build is warning-free.

## Findings: closing the measured shim gaps (2026-09-26)

With a scope decided ([`standard.md`](standard.md)) and the imported-symbol
corpus measured ([`shim-coverage.md`](shim-coverage.md)), the eight symbols
that were genuinely imported but not intercepted are now covered in
`native/path-redirect.c`: the legacy `__xstat`/`__lxstat` entry points (and
their `*64` forms — `__fxstat` is fd-based and needs no redirect);
`sendmsg`, rewriting the AF_UNIX address in a copied `msghdr` the same way
`sendto` does; `lutimes`; `mkstemps`/`mkostemps`, with the same
template copy-back as `mkstemp`; and `eaccess`/`euidaccess`, `setmntent`.
`scandir`/`scandir64` were also promoted from "covered indirectly" to
explicit redirects: glibc walks the directory with an internal `opendir`
that does not pass through the interposed symbol, so relying on the
one-level-down redirect was an assumption, not a fact.

`tests/shim-libc/run.sh` grew to 46 asserted rewrites and passes on-device;
`docs/coverage/path-symbols.tsv` now marks every imported path-taking symbol
`shim` except the NSS lookups (untested), `glob`/`glob64` (indirect),
`mount`/`umount2`/`chroot` (admin), and the raw-`syscall()` boundary.

The NSS question was then tested and answered: **not redirected, and not
fixable at the libc layer.** With a fake `$INSTDIR/etc/passwd` holding
`dnshim:54321`, `getpwnam("dnshim")` and `getpwuid(54321)` returned
`NOTFOUND`, and `getgrgid(54321)` returned the real Android group
`all_a4321`, and `DN_REDIRECT_DEBUG=1` produced no rewrite line for
`nsswitch.conf`/`passwd`/`group`/`hosts`/`resolv.conf` at all — every open
is internal. `libc.so.6` defines `_nss_files_*`/`_nss_dns_*` itself; the
bundled `libnss_files.so.2`/`libnss_dns.so.2` are empty ABI stubs that
glibc never `dlopen`s. Stock Debian glibc is identical (its `libc.so.6`
defines `_nss_files_getpwnam`; its `libnss_files.so.2` is a stub), so this
is upstream glibc design, not Termux. The opens use the private
`__open_nocancel`/`__open64_nocancel` (`GLIBC_PRIVATE`), and even the
`nsswitch.conf` dispatch is internal, so a custom NSS module cannot be
selected either.

It is not unfixable, just not here: the syscall tracer catches the `openat`
before any of this and covers NSS, raw `syscall()` and static binaries
uniformly. The only libc-layer alternative — interposing the public
`getpwnam`/`getpwuid_r`/… and reimplementing the `files` lookup — is a
partial reimplementation and not worth it against the tracer. So the shim is
complete at its layer; the remaining gaps all belong to the tracer.

## Findings: fusion-no-prefix, `update-alternatives` doesn't get the shim (2026-09-26)

`fusion-no-prefix` (branch) drops the separate sandboxed prefix entirely:
packages install straight into Termux's own live `$PREFIX`, via Termux's own
real `dpkg` with `arm64` added as a foreign architecture
(`dpkg --add-architecture arm64`). Tested by hand against `figlet:arm64`
(has maintainer scripts; `sysvbanner:arm64`, no scripts, already installs
and runs cleanly with zero extra steps — the baseline this compares
against).

### Bug 1: `dpkg --configure` crashes with SIGSYS — dpkg's own `chroot()`

`strace -f` on a hung `-i` traced it exactly: the crashing process calls
`chroot()` before running the postinst, and Android's seccomp filter always
blocks `chroot()` regardless of uid. Fix: pass `--force-script-chrootless`
to `dpkg`. Confirmed: the crash is gone and the postinst runs.

### Bug 2: `dn-launch`'s self-location math is wrong for a flat prefix

`dn-launch.c` derives `$INSTDIR` two ways: an explicit override
(`DN_INSTDIR` + `DN_FUSE_SHIM` env vars, trusted completely) or, absent
those, `/proc/self/exe` with three path components stripped — math written
for the classic design's own `$INSTDIR/usr/bin/dn-shell` layout. Fusion mode
has no such nested tree (the postinst's interpreter binary lived directly
under a test scratchpad dir), so the fallback computes a nonsense path and
the maintainer script's own subprocesses fail to load the shim (`CANNOT LINK
EXECUTABLE ... library ... not found`). The explicit-override path already
exists for exactly this reason (see the comment above it in
`native/dn-launch.c`) — it just has to actually be used: export
`DN_INSTDIR="$PREFIX"` and `DN_FUSE_SHIM=<path to path-redirect.so>` before
invoking `dpkg --configure`, don't rely on the fallback.

### Non-bug, confirmed by reading the code first: no second shim is needed

Both `dpkg` and `update-alternatives` on Termux are its own native Bionic
builds (`file`: `interpreter /system/bin/linker64, built by NDK r27`), not
glibc — confirmed before touching anything further, per the standing rule
about researching a tool's real behavior before probing it live. This
matters because `path-redirect.so` is a glibc-only `LD_PRELOAD` shim; the
existing `execve()` dispatch in `native/path-redirect.c` already detects a
non-glibc exec target (`target_is_glibc()`) and strips `LD_PRELOAD` before
handing it to a Bionic child (`bionic_env()`) — by design, not a gap.
`update-alternatives` runs with no shim at all, and that's correct: the
"two shims, static vs. dynamic" split the classic design needed is already
just this one dispatch, reused as-is.

### Bug 3: `update-alternatives` needs a real nested `usr/`, and fusion mode has none

With bug 1 and 2 fixed, the postinst's
`update-alternatives --install /usr/bin/figlet figlet /usr/bin/figlet-figlet …`
still hard-failed:

```
update-alternatives: error: alternative path /data/data/com.termux/files/usr/usr/bin/figlet-figlet doesn't exist
```

— a **doubled** `usr/usr`, from `update-alternatives` joining its own root
(`$PREFIX`, i.e. `DPKG_ROOT`, which dpkg auto-exports to maintainer scripts
from `--instdir`) with the literal `/usr/bin/figlet-figlet` argument the
(unmodified) Debian postinst script passes. The classic design and
`sudo-less` never hit this: both give the prefix a *real* nested `usr/`
(`sudo-less` via a mount-namespace view where `$PREFIX/usr` and `/usr` are
bind-mounted to the same tree; the classic prefix design lays one out on
disk), so the join was always valid there. Fusion mode's whole premise is no
nested `usr/` — this is the one place that premise collides with a
Termux-native tool's own path handling, and no `LD_PRELOAD` shim can catch
it (previous section).

**Fix, reusing what already works instead of patching dpkg-native tools**:
`ln -s . "$PREFIX/usr"` — a self-referential symlink. `$PREFIX/usr/bin/x`
now really does resolve to `$PREFIX/bin/x` on disk, no code changes
anywhere. With that in place, the same `dpkg --configure figlet` run
completes with no error and `Status: install ok installed`.

### Bug 4 (root-caused via `strace`, not guessed): the alternative was still functionally dangling

Even on the clean "installed" run, `$PREFIX/bin/figlet` (created via the
`usr` symlink) pointed at `$PREFIX/etc/alternatives/figlet`, and that target
was never created. Flag-probing `update-alternatives --list`/`--display`
by hand gave inconsistent errors depending on `--root`/`DPKG_ROOT` — the
wrong way to chase this (this repo's own standing rule: research a tool's
real behavior before probing it live), so this was re-done with `strace -f`
on the actual syscalls instead, which found **two separate, stackable
bugs**, both already known in shape from elsewhere in this repo:

1. **`--altdir` double-prefixes, same as the documented `mawk` bug.**
   `DPKG_ROOT=$PREFIX` alone (matching what `dpkg` auto-exports to a
   maintainer script) makes `update-alternatives` join `DPKG_ROOT` with its
   own compiled-in *absolute* `--altdir`/`--admindir` defaults —
   `$PREFIX/$PREFIX/etc/alternatives/…` — traced directly:
   `symlinkat("/usr/bin/figlet-figlet", …, ".../usr/data/data/com.termux/files/usr/etc/alternatives/figlet.dpkg-tmp")`.
   The master link (`$PREFIX/bin/figlet`, via the `usr` symlink) ends up
   correct while the file it points at gets physically written to that
   doubled, unrelated path — hence "dangling" despite a clean install.
   `commit af6500b` on the classic-prefix branch already root-caused and
   fixed the *general* shape of this (`--admindir`/`--altdir` need forcing
   as explicit, space-separated flags, not `=`-joined, and not left to
   `DPKG_ROOT` alone) — fusion mode hadn't picked that fix up yet. Verified
   fix: pass both explicitly —
   `--altdir "$PREFIX/etc/alternatives" --admindir "$PREFIX/var/lib/dpkg/alternatives"`
   alongside `DPKG_ROOT="$PREFIX"` — and the files land in the right place,
   traced clean (`symlinkat`/`renameat2` on the correct, single-prefixed
   paths, `exit=0`).

2. **Fixing (1) makes it write *portable*, host-root-absolute symlink
   targets — `/etc/alternatives/figlet`, `/usr/bin/figlet-figlet`, no
   `$PREFIX` — which is exactly `sudo-less`'s own documented
   `0102-relative-symlinks` problem** (`docs/apt-dpkg-port.md:145-155`,
   quoted earlier in this log): valid where a mount-namespace view makes
   `/etc` and `$PREFIX/etc` the same tree, wrong here, where there is no
   view and the kernel chases a multi-hop symlink in one `execve`/`openat`
   — never re-entering userspace per hop — so `LD_PRELOAD` cannot rewrite
   the intermediate targets. This is also exactly what this repo's own
   `normalize-symlinks.sh` already exists to fix for the classic design
   (rewrite an absolute target under `usr/etc/var/opt/bin/sbin` to a
   relative one, so kernel resolution never leaves the prefix) — it just
   assumes a real nested `usr/`, which fusion mode's `$PREFIX$target`
   computation would need to strip the same way `path-redirect.c`'s
   `DN_FUSE_USR` does before this applies cleanly here.

**Fixed and verified this session**, reusing both mechanisms the classic
branch already built for this exact bug class rather than inventing new
ones:

- `scripts/fuse-runtime.sh` (new) generates an `update-alternatives`
  wrapper at `$INSTDIR/lib/deb-native/fusion-bin/update-alternatives` — an
  `af6500b`-style fix (force `--altdir`/`--admindir` explicitly, space-
  separated), just placed in its own directory instead of
  `$INSTDIR/usr/bin`: that path aliases to Termux's own real `bin/` in
  fusion mode (no separate sandbox tree), so a same-named file there would
  replace Termux's own binary system-wide, not shadow it. `native/dn-launch.c`'s
  fuse-mode `PATH` now puts `fusion-bin` first so maintainer scripts find
  it ahead of the real one.
- `scripts/normalize-symlinks.sh` gained two opt-in env vars:
  `NORMALIZE_FUSE_USR=1` (strip a leading `/usr` before joining with ROOT,
  matching `path-redirect.c`'s `DN_FUSE_USR`) and `NORMALIZE_SCAN_DIRS`
  (scan only the given ROOT-relative dirs, non-recursive, instead of all of
  ROOT — fusion's ROOT is Termux's own live, shared `$PREFIX`, so a full
  recursive scan would touch every symlink on the system, not just a
  package's own; classic design's ROOT is its own small sandbox, where the
  original full-tree behavior is intentional and stays the default).

Verified end to end, from a clean `--remove-all figlet`: the postinst
(rebuilt `dn-launch`, invoked directly since dpkg won't re-run configure on
an already-`ii` package) reports success with the new wrapper active, disk
state shows both alternatives links landing in the *correct*, single-
prefixed location this time — then, before normalizing,
`$PREFIX/usr/bin/figlet` still fails (`No such file or directory`): the
links are correctly placed but their *targets* are host-root-absolute
(`/etc/alternatives/figlet`, `/usr/bin/figlet-figlet`, no `$PREFIX`) —
exactly the predicted problem 2. Running
`NORMALIZE_FUSE_USR=1 NORMALIZE_SCAN_DIRS="etc/alternatives bin share/man/man6" normalize-symlinks.sh "$PREFIX"`
rewrites them to correct relative targets
(`../etc/alternatives/figlet`, `../../bin/figlet-figlet`), and `grun
$PREFIX/bin/figlet-figlet` then executes — following the *entire* chain
(`$PREFIX/bin/figlet` → `.../etc/alternatives/figlet` →
`.../bin/figlet-figlet`) to the exact right, real, foreign-arch ELF binary.
Bug 4 is closed.

### New, distinct finding: the runtime stage was never adapted for fusion mode at all

`grun $PREFIX/bin/figlet-figlet` gets past loading (interpreter
`/lib/ld-linux-aarch64.so.1`, a real Debian glibc arm64 binary) but fails
at its own first `open()`: `Unable to open font file`, even though
`$PREFIX/share/figlet/standard.flf` genuinely exists. `DN_REDIRECT_DEBUG=1`
under `grun` prints **zero** rewrite lines — the shim never loads into the
child process at all, unlike under `dn-launch` (install stage), where it
demonstrably does. `grun` most likely manages its own `LD_PRELOAD`/glibc
environment and doesn't pass ours through. This is what install-flow.md
calls out as a *separate* concern from install (`dn-run`, launchers,
`patch-elfs.sh` in the classic design) — fusion mode has only ever
exercised the install stage (`dn-launch`, maintainer scripts) so far, never
actually running an installed foreign binary. Not investigated yet; next
session's next question.

### Live system state as of this session

- `figlet:arm64`: `Status: install ok installed`, and the alternative now
  genuinely resolves to the right file (previous section) — actually
  *running* it still doesn't work (next section).
- `$PREFIX/usr` remains a real symlink (`-> .`) on this device — permanent,
  deliberate, depended on by bug 3's fix.
- `$PREFIX/lib/deb-native/fusion-bin/update-alternatives` (new) — a
  permanent addition to this device, harmless to Termux itself (its own
  directory, not on anyone else's `PATH`).
- `sysvbanner:arm64`: genuinely installed and working, untouched.
- `arm64` remains a registered foreign architecture in Termux's dpkg.

