# docs/log/findings/

> Template: [`templates/readme.template.md`](../../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

The engineering log for the prototype, one entry per file instead of
one long chronological doc (split 2026-10-02 from the former
`findings.md`, kept in full — nothing summarized away), for
searchability and so each entry carries its own **Impact** label:
`Repo change` (a fix/patch actually landed), `Isolated` (a
confirmation or survey, no code changed), or `Open gap` (a real gap
found, flagged, not yet fixed as of that entry). Chronological order
below, oldest first, matching the original file's order.

- [`first-working-prototype.md`](first-working-prototype.md) — 2026-09-25,
  **Open gap**: the first real Debian `.deb` (`hello`) installed and ran
  end to end; Termux's own glibc side-install turned out to already solve
  the ELF-interpreter problem, re-scoping the project; two blockers
  worked around (`--force-architecture`, `--force-depends`), not
  properly fixed yet.
- [`proper-base-bootstrap.md`](proper-base-bootstrap.md) — 2026-09-25,
  **Open gap**: several real bugs fixed (batched unpack/configure vs.
  `Pre-Depends`, stale cached `.deb`s, double-patching); `chown`
  permission errors and commands forked outside the shim's reach left
  open at session end.
- [`glibc-dash-maintainer-shell.md`](glibc-dash-maintainer-shell.md) —
  2026-09-25, **Open gap**: maintainer scripts rewritten to run under a
  real Debian `dash`, several real `LD_PRELOAD`/`PATH` bugs fixed;
  `debconf`/`cdebconf`'s deeper case (`preinst` running before any patch
  step touches it) found, not yet closed.
- [`hard-package-ruby-adsf.md`](hard-package-ruby-adsf.md) — 2026-09-25,
  **Open gap**: fixed a real apt-invocation-shape bug (unpack/patch/configure
  ordering); `debconf`/`cdebconf` confirmed a genuinely hard, named,
  accepted-as-open subsystem rather than a bug in this project.
- [`sed-delimiter-bug.md`](sed-delimiter-bug.md) — 2026-09-25,
  **Repo change**: a `sed` delimiter bug that silently aborted
  maintainer-script patching for every file after the first failure,
  found and fixed — the single highest-value bug of its day.
- [`complete-base-bootstrap.md`](complete-base-bootstrap.md) —
  2026-09-25 (PM), **Repo change**: seven real root causes found and
  fixed on a fresh prefix (the maintainer-script interpreter, the
  chicken-and-egg bootstrap order, `chdir`, `grun` self-corruption, the
  shim's own install path, `execvp` bypassing the shim, a missing
  `dpkg-realpath` data file) — closes the two gaps the previous two
  entries left open.
- [`first-random-sample-survey.md`](first-random-sample-survey.md) —
  2026-09-25, **Isolated**: a 30-package random survey (2/30 installed)
  diagnosing the real cause (no dependency installer at all) without
  fixing it — the fix is the next entry.
- [`wiring-real-apt.md`](wiring-real-apt.md) — 2026-09-25,
  **Open gap**: real `apt` wired up, 2/30 → 10/30; surfaces three new
  unresolved classes (shim doesn't reach maintainer scripts, dpkg
  hardlinks fail here, `unresolvable` exact-version deps).
- [`shim-performance.md`](shim-performance.md) — 2026-09-25,
  **Repo change**: the `LD_PRELOAD` shim's hot path optimized (cached
  env, branch dispatch, `memcpy`, `-O2` build) — no stable end-to-end
  number claimed, but the code changes are unconditional wins.
- **Platform sandbox limits, by direct probe** — 2026-09-26: moved
  entirely into [`../../spec/android-platform.md`](../../spec/android-platform.md)
  ("Device probe: sandbox limits confirmed directly") at the time; no
  separate file here, just this pointer.
- [`finishing-libc-level-shim.md`](finishing-libc-level-shim.md) —
  2026-09-26, **Repo change**: closed every item a code review flagged
  for the libc-level shim; remaining territory belongs to the tracer,
  tracked elsewhere.
- [`patchelf-et-exec-runpath.md`](patchelf-et-exec-runpath.md) —
  2026-09-30, **Repo change**: root-caused `patchelf` corrupting an
  `ET_EXEC` binary's program headers via clean-room comparison; fixed
  by moving `RUNPATH` into `ld-dn`'s per-launch environment instead of
  a static per-file patch.
- [`apt-install-gcc-end-to-end.md`](apt-install-gcc-end-to-end.md) —
  2026-10-01, **Open gap**: two real bugs found (one fixed — apt's
  `PATH` for maintainer scripts; one worked around only, not
  architecturally fixed — a bootstrapped prefix breaking when
  `scripts/` moves); stand-ins found never held, fixed.
- [`libc6-dev-gap-closed.md`](libc6-dev-gap-closed.md) — 2026-10-01,
  **Repo change**: closed the `libc6-dev` "no installation candidate"
  gap by removing a version-bump suffix instead of building a custom
  package.
- [`gcc-hello-pt-interp-gap.md`](gcc-hello-pt-interp-gap.md) —
  2026-10-01, **Open gap**: the shim's `/lib`/`/bin`/`/sbin` gap fixed;
  a separate, deeper `PT_INTERP`-is-kernel-level wall for binaries
  built *inside* the prefix flagged, not fixed, as of this entry.
- [`server-stack-unmodified.md`](server-stack-unmodified.md) —
  2026-10-02, **Isolated**: sockets, SQLite WAL, `uvloop`/`httptools`,
  and Alembic all confirmed working with no changes needed — credited
  to Termux's existing glibc side-install, not a new deb-native
  capability, kept as a regression baseline for when 0.5.0's own
  `libc6` becomes the default.
- [`fused-shim-self-derives-prefix.md`](fused-shim-self-derives-prefix.md) —
  2026-10-03, **Repo change**: for the `dn-glibc` fused loader, the shim now
  derives its prefix from its own load path (`dladdr`) with no injected env
  — proven on `fe2` (redirect works with `DN_INSTDIR` unset); full loader
  integration (own-glibc + rebuilt shim) deferred.
