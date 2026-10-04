# The classic design: separate prefix, static wrappers, no namespaces

<!-- template: templates/docs.template.md -->

The pre-0.2.0 approach: reuse Termux's apt/dpkg via plain relocation
flags (no own database), and static per-binary wrappers instead of a
kernel view. Superseded in large part by
[`design.md`](../spec/design.md)'s 0.2.0 pivot (a self-contained prefix with
its own apt/dpkg database) and by the shim
([`path-shim.md`](../spec/shim/path-shim.md), which covers hardcoded paths that
this section's wrapper-generation direction never got built for) —
kept as the record of the proposal and the research it did settle
(Direction 3 is still the project's only writing on services).

## Contents

- [Install path: reuse Termux's apt/dpkg, don't fork them](#install-path-reuse-termuxs-aptdpkg-dont-fork-them)
- [Direction 2: static per-binary wrappers (replaces the "view")](#direction-2-static-per-binary-wrappers-replaces-the-view)
- [Triggering Direction 2's wrapper generation: apt/dpkg hooks, not a patch](#triggering-direction-2s-wrapper-generation-aptdpkg-hooks-not-a-patch)
- [Direction 3 (research): services without systemd](#direction-3-research-services-without-systemd)

## Related docs

- [`design.md`](../spec/design.md) — the doc this was split out of; the 0.2.0
  pivot that superseded most of the install-path content here.
- [`path-shim.md`](../spec/shim/path-shim.md) — the shim that replaced Direction 2's
  wrapper-generation approach for hardcoded-path packages.
- [`native-reuse.md`](native-reuse.md) — the two-layer database idea
  this doc's "Install path" section introduces.
- [`prior-art.md`](prior-art.md) — sudo-less's own view/wrapper mechanism
  this doc's directions are a static substitute for.

## Install path: reuse Termux's apt/dpkg, don't fork them


Status: **architecture decision.** Not implemented yet.

### Decision

Do **not** fork or patch apt/dpkg source, unlike sudo-less. Use Termux's
own `apt`/`dpkg` binaries as-is, pointed at a separate prefix via their
existing relocation flags and a custom `apt.conf`.

### Why sudo-less had to fork, and why we don't

sudo-less runs on a real Debian host, where stock dpkg refuses to operate
without root ("requested operation requires superuser privilege"), calls
`chown`, and checks for `ldconfig` on `PATH` at startup. Their patches
(`0001-no-superuser-check`, `0002-no-chown`, `0003-no-ldconfig-check`)
remove exactly those checks — and per `docs/apt-dpkg-port.md` in sudo-less,
those patches are themselves lifted from Termux's own `termux-packages`
patch set, just made unconditional instead of `#ifndef __ANDROID__`.

We're running *in* Termux. Termux's apt/dpkg already ship the
`__ANDROID__`-guarded version of those same changes, built in. There is
nothing left to remove.

### What to do instead

#### dpkg: relocate with existing flags, no chroot

```sh
dpkg --instdir="$NEWPREFIX" \
     --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-script-chrootless \
     --force-not-root \
     -i pkg.deb
```

This is stock dpkg functionality (`--instdir`, `--force-script-chrootless`,
`--force-not-root`), not a patch. It's also exactly what sudo-less itself
used *before* building "the view" (`apt-dpkg-port.md`, point 6: "Before the
view: `--instdir=$PREFIX` and `--force-script-chrootless`").

Consequence of skipping the view (accepted limitation, see
[`path-shim.md`](../spec/shim/path-shim.md)): maintainer
scripts run via `--force-script-chrootless` execute directly with Termux's
own `/bin/sh`, seeing real absolute paths (`/etc/foo`) that do **not**
resolve into `$NEWPREFIX` — only `$DPKG_ROOT` (which dpkg exports to
scripts) tells a well-behaved script where the real files are. A script
that assumes `/etc/foo` unconditionally means the prefix will write to (or
fail to write to) the real root instead. This is the same class of gap
Direction 2 already flags, not a new one.

#### apt: point Dir::* at the new prefix, no patch

A dedicated `apt.conf` (not committed to Termux's own):

```
Dir::State "NEWPREFIX/var/lib/apt";
Dir::State::status "NEWPREFIX/var/lib/dpkg/status";
Dir::Cache "NEWPREFIX/var/cache/apt";
Dir::Etc "NEWPREFIX/etc/apt";
RootDir "NEWPREFIX";
```

Loaded via `apt -o Dir::... ` overrides or `APT_CONFIG=path/to/this.conf`.
This mirrors sudo-less's generated `00local-prefix` — a config file, not a
source change.

`$NEWPREFIX` must be **separate from Termux's own `$PREFIX`**
(`/data/data/com.termux/files/usr`) — reusing it would let this project's
installs corrupt Termux's own package database. Use something like
`~/.local` (matching sudo-less's own default) or a dedicated directory.

### The two-layer database: seeding runs backwards here

sudo-less seeds the prefix's dpkg status from the **host's**
`/var/lib/dpkg/status` so apt treats already-present system libraries
(glibc included) as satisfied and only installs leaf packages. Their host
already has glibc — that's the whole premise.

Termux has no glibc at all. For glibc arm64 `.deb`s, we specifically *want*
apt to pull in the full `libc6` dependency chain into `$NEWPREFIX` — that's
the actual point of this project (glibc side-install, same rootfs
glibc-runner has been manually pointing patched ELF interpreters at).

Seeding may still be useful for the *other* direction: packages Termux
already provides a Bionic-native equivalent for (`zlib1g`, `libssl3`,
etc.), where duplicating a glibc copy into `$NEWPREFIX` would be wasted
disk/maintenance for no benefit if the plan ends up being "run glibc
binaries against the glibc side-install's own full lib chain" rather than
"mix and match Bionic and glibc libs" — mixing the two would be an ABI
minefield anyway, so the realistic default is: **don't seed from Termux's
own package set at all**; let `$NEWPREFIX` be a self-contained glibc tree,
and only reconsider seeding if disk footprint or duplicate maintenance
becomes an actual problem.

### Maintainer scripts calling root-only or missing helpers

Same caveat sudo-less documents (`apt-dpkg-port.md`, "Not patched, on
purpose"): scripts calling `ldconfig`, `update-alternatives`, `systemctl`,
`py3compile`, etc. need either a shim on `PATH` inside `$NEWPREFIX/bin` or
the package excluded. Not designed yet — first need a real sample of
maintainer scripts from target packages to see which helpers actually get
called (open item, same as the coverage survey later measured in
[`shim-coverage.md`](../spec/shim/shim-coverage.md)).

### Open work

- [ ] Confirm Termux's shipped `apt`/`dpkg` versions support all the flags
      above (`--force-script-chrootless` in particular) — check
      `dpkg --version` / `man dpkg` on-device rather than assuming version
      parity with sudo-less's apt 3.3.3 / dpkg 1.23.11.
- [ ] Verify apt's signature verifier: sudo-less relies on the host's `sqv`
      (apt 3.x default); confirm what Termux's apt build uses (`sqv` or the
      older `gpgv`) and that it's present.
- [ ] Decide `$NEWPREFIX`'s location and whether it's user-configurable
      from day one or hardcoded for the R&D phase.
- [ ] Test `dpkg --instdir` + `--force-script-chrootless` against one real,
      simple glibc arm64 `.deb` (e.g. `hello`) end to end before attempting
      anything with maintainer scripts or dependencies.

## Direction 2: static per-binary wrappers (replaces the "view")


Status: **design, not implemented.**

### Problem this replaces

sudo-less's "view" (`docs/view.md` in sudo-less) makes a package's
hardcoded absolute paths (`/etc/foo.conf`, `/usr/share/foo/templates`)
resolve correctly at run time by overlaying the prefix onto the real `/usr
/etc /var /opt` inside a private mount namespace, live, for the duration of
the call. That needs `unshare(CLONE_NEWUSER)` + unprivileged overlayfs,
both assumed blocked under Termux's SELinux domain (see
[`path-shim.md`](../spec/shim/path-shim.md)).

### Approach

Do the same job **ahead of time, per binary, with no namespace**: at
install time, for each program `prefix-wrap`'s heuristics flag as needing
path help, generate a fixed script (or patch the binary directly) that
resolves its paths against the prefix explicitly, instead of relying on
`/etc/foo.conf` transparently meaning the prefix's copy.

Concretely, by failure mode (same table as sudo-less's `view.md`):

| why the binary needs help | static fix |
|---|---|
| interpreter shebang not on host (`#!/usr/bin/ruby`, host only has Termux's `ruby`) | rewrite the shebang at install time to the actual interpreter path in the prefix, or wrap with `exec $PREFIX/usr/bin/ruby "$0" "$@"` |
| interpreter only searches compiled-in module paths (Python/Perl/Node/...) | wrapper sets the interpreter's own search-path env var (`PYTHONPATH`, `PERL5LIB`, `NODE_PATH`, ...) to the prefix's copy before `exec`ing — this is exactly the case sudo-less's own docs call out as **not** solvable by env vars *for the view's other cases*, but it's the right tool specifically for module search paths |
| `ldd` can't find a library the package ships | wrapper sets `LD_LIBRARY_PATH` to the prefix's lib dir before `exec` — same caveat as above: fine for this one case, not a general substitute for the view |
| ELF binary is a glibc build, needs glibc-runner | wrapper (or the binary's patched ELF interpreter directly, per the existing manual glibc-runner method) invokes it against the glibc side-install |
| binary/script has a hardcoded absolute path to its own data (`/usr/share/figlet`, `/etc/redis/redis.conf`) it reads directly, not through a library call the above env vars cover | **solved** — see [`path-shim.md`](../spec/shim/path-shim.md): our own shim (`native/path-redirect.c`) intercepts `open`/`openat`/`fopen`/`stat`/`fstatat` and rewrites the path, verified against `figlet`'s real `/usr/share/figlet` lookup |

### What this used to not solve (now closed)

The view's whole point is Debian packages assume `/` is real. A static
wrapper only helps for the *specific, enumerable* ways a program looks
things up (interpreter search paths, dynamic linker search paths,
shebangs). A binary that does `open("/etc/foo.conf")` directly in its own
C code, with no env var and no CLI flag to redirect it, used to have no
static fix short of binary-patching the literal path string (only works if
the replacement is the same length or shorter) — **until
[`path-shim.md`](../spec/shim/path-shim.md)'s shim, now built and verified**.
Binary-patching the string remains the fallback for a statically-linked
binary (no dynamic libc calls to intercept), which the shim genuinely
cannot reach.

Per sudo-less's own survey (`survey-2026-09.md`, referenced from
`view.md`): ~73% of packages need **no** path help at all and just run from
the prefix's `bin/` directly. Of the remaining ~27%, an unmeasured fraction
falls into the "hardcoded path, no env var" bucket this direction can't
reach. **Needs its own survey against real glibc arm64 `.deb`s before
claiming a coverage number** — do not assume sudo-less's 73%/27% split
transfers; it was measured on Debian's package set with Debian's own
`/usr` layout assumptions, not against Termux's prefix.

See "Triggering Direction 2's wrapper generation" below for how this
detection/generation step gets triggered automatically after an install (apt's
`DPkg::Post-Invoke` plus a `dpkg` wrapper script, not a patch).

### Open work

- [ ] Run `prefix-wrap`'s detection heuristic (or a reimplementation of it)
      against a real sample of glibc arm64 `.deb`s to get an actual
      coverage number for "wrapper suffices" vs. "needs a path-virtualization
      layer neither direction handles yet".
- [x] ~~Decide whether the shim is worth building~~ — built,
      see [`path-shim.md`](../spec/shim/path-shim.md).
- [ ] Wire the shim's env vars (`DN_REDIRECT_FROM`/`_TO`) into the
      wrapper-script generation this doc describes, instead of setting
      them by hand as done for the `figlet` test.
- [ ] Decide whether the remainder (statically-linked binaries, unreachable
      by any dynamic-call interception) is small enough to just exclude
      (same as sudo-less excludes root-only-maintainer-script packages).
- [ ] Reuse or reimplement `prefix-wrap`'s wrapper-script generation
      (`$PREFIX/bin/<name>` script recorded per-package so it's removed
      with the package) — this part has no namespace dependency and can
      likely be ported near-verbatim.

## Triggering Direction 2's wrapper generation: apt/dpkg hooks, not a patch


Status: **design, not implemented.** Depends on "Direction 2: static
per-binary wrappers" above (what gets generated) and "Install path:
reuse Termux's apt/dpkg" above (the prefix layout it runs against).

### Decision

Run the wrapper-generation step (detect which newly-installed binaries
need a shebang rewrite / env-var wrapper / glibc-runner ELF patch, per
"Direction 2" above) automatically after every install, using two
stock hook points — no apt/dpkg source change:

1. **`DPkg::Post-Invoke`** in a prefix-scoped `apt.conf.d` snippet, for
   installs done through `apt-get install`.
2. **A `dpkg` wrapper script** on `$NEWPREFIX/bin`, ahead of the real dpkg
   on `PATH`, for direct `dpkg -i` calls that bypass apt entirely.

This mirrors sudo-less's own mechanism (`docs/view.md`, "How programs get
there"): their `prefix-wrap` runs via `apt.conf.d/02integrate.in`
(`DPkg::Post-Invoke`-style) after apt-driven installs, and via their own
`$PREFIX/bin/dpkg` wrapper "after a dpkg run apt did not make" — i.e. they
hit the same gap and solved it the same way.

### Why both are needed (the gap that breaks if only one exists)

- `DPkg::Post-Invoke` is an **apt** config directive, honored only when
  **apt** invokes dpkg. It does nothing for a bare `dpkg -i pkg.deb`
  typed directly, or run by some other script — dpkg has no equivalent
  generic "ran to completion" hook of its own (dpkg *triggers* exist, but
  they're package-declared interest in specific paths, not a general
  post-run hook, and would need the packages themselves to declare
  interest in something they have no reason to know about).
- A `$NEWPREFIX/bin/dpkg` wrapper that shells out to the real dpkg and then
  runs the same detection step closes that gap, but only if it stays ahead
  of the real dpkg on `PATH` — the ordering this project already relies on
  for `$NEWPREFIX/bin` (see "Install path" above).
- Neither alone is sufficient: apt-driven installs go through both apt
  *and* the dpkg it calls internally, so the dpkg wrapper's own hook logic
  needs a guard against double-running (an env var set by the apt hook's
  caller, or a lock/marker file for "already ran for this transaction") —
  otherwise a single `apt-get install` would trigger wrapper generation
  twice, redundant but not obviously wrong, so worth avoiding cleanly
  rather than leaving as a known-harmless quirk.

### Config sketch

```
# $NEWPREFIX/etc/apt/apt.conf.d/90wrap-glibc
DPkg::Post-Invoke {
    "test -n \"$DN_DPKG_WRAPPER_RAN\" || $NEWPREFIX/lib/deb-native/wrap-new-binaries";
};
```

```sh
#!/bin/sh
# $NEWPREFIX/bin/dpkg — wrapper, not a patch
export DN_DPKG_WRAPPER_RAN=1
"$NEWPREFIX/lib/deb-native/real-dpkg" "$@"
status=$?
"$NEWPREFIX/lib/deb-native/wrap-new-binaries"
exit "$status"
```

(Names/paths illustrative — not yet decided where the real dpkg binary
gets moved to so the wrapper can claim the `dpkg` name on `PATH`, likely
`$NEWPREFIX/lib/deb-native/real-dpkg` or similar, matching how
sudo-less relocates its own wrapped binary.)

### What "wrap-new-binaries" needs to know

To avoid re-scanning every binary in the prefix on every invoke, it needs
to know what changed since the last run — sudo-less's `prefix-wrap` scopes
itself to "each package installed or changed since the last run" via a
stamp file. Same approach here: compare `$NEWPREFIX/var/lib/dpkg/status`
mtime (or a package-list diff) against a stamp under
`$NEWPREFIX/.deb-native/wrap.stamp`, matching sudo-less's
`$PREFIX/.sudo-less/view/mirror.stamp` pattern referenced in `view.md`.

### Open work

- [ ] Confirm `DPkg::Post-Invoke` fires reliably when apt's `Dir::*` is
      pointed at `$NEWPREFIX` via the custom `apt.conf` from
      "Install path" above — not yet tested on-device.
- [ ] Decide the double-run guard mechanism (env var vs. lock file vs.
      transaction id) once the wrapper script itself exists to test
      against.
- [ ] Decide where the real dpkg binary lives once wrapped (can't keep the
      name `dpkg` in the same directory as the wrapper).

## Direction 3 (research): services without systemd


Status: **research only, no design yet.** This doc records what's known and
what's still open — not a plan to implement against.

### What sudo-less does

`docs/services.md` in sudo-less (not yet read in full — follow-up) covers
translating a package's systemd unit into a `systemd --user` unit that runs
inside a "service view": a private mount-namespace view built fresh per
start, so the daemon reads its config from `/etc/foo/foo.conf` and writes
state to `/var/lib/foo` exactly as it would on real Debian, landing in the
prefix. `$XDG_RUNTIME_DIR/sudo-less/run` stands in for `/run`.

### Why it doesn't transfer

Two independent blockers, not one:

1. **No systemd on Termux at all** — no PID 1 systemd, no `systemctl`, no
   user manager, no unit files, no cgroups in the relevant sense. Termux's
   native service supervisor is [`termux-services`](https://github.com/termux/termux-services),
   built on `runit`: services are directories under
   `$PREFIX/var/service/<name>/run` (an executable script `runsv`
   supervises), enabled/disabled via `sv-enable`/`sv-disable`.
2. **No service view** — even if a runit script stood in for the systemd
   unit, the config/state/`/run` redirection sudo-less's service view
   provides depends on the same blocked mount-namespace mechanism as
   Direction 2 is working around. A runit script alone doesn't solve path
   resolution.

### Open questions (not yet answered)

- Does a `.deb` package that ships a systemd unit under
  `/lib/systemd/system/*.service` carry enough information in that unit
  file (`ExecStart=`, `ExecStop=`, `User=`, environment) to mechanically
  generate a runit `run` script, the way sudo-less mechanically generates a
  user unit? Needs reading actual systemd unit files from real packages to
  judge — not researched yet.
- Whether Direction 2's static wrappers (env vars for config/data paths)
  are enough for a *daemon* the same way they might be for a CLI tool, or
  whether daemons disproportionately fall into the "hardcoded absolute
  path, no env var" bucket "Direction 2" above flags as unsolved —
  daemons reading `/etc/<name>/<name>.conf` directly is extremely common
  and was one of `view.md`'s own motivating examples (`redis.conf`). If so,
  Direction 3 may be blocked on the same open problem as Direction 2's
  gap, not just on "no systemd".
- Whether `termux-services`/runit's process supervision model (long-running
  foreground process under `runsv`, restart on exit) is even the right fit
  for packages that assume `systemd`'s notify/socket-activation protocols
  (`Type=notify`, `sd_notify()`) — a package relying on those would need
  either a shim for `sd_notify` or exclusion, unresearched which is more
  common.

### Non-conclusion

This direction is **not** "port sudo-less's service view to runit" — that
undersells the problem, since the actual hard part (path resolution for a
daemon that expects `/etc/foo` and `/var/lib/foo` to be real) is shared
with Direction 2's unsolved gap, not something runit vs. systemd changes
either way. Next step is reading sudo-less's `docs/services.md` in full and
picking one real daemon package to trace by hand (what paths does it read,
what would a runit script + Direction 2's wrapper actually cover) before
writing any design here.
