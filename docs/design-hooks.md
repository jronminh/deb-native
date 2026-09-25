# Triggering Direction 2's wrapper generation: apt/dpkg hooks, not a patch

Status: **design, not implemented.** Depends on
[`design-static-wrappers.md`](design-static-wrappers.md) (what gets
generated) and [`design-install-path.md`](design-install-path.md) (the
prefix layout it runs against).

## Decision

Run the wrapper-generation step (detect which newly-installed binaries
need a shebang rewrite / env-var wrapper / glibc-runner ELF patch, per
`design-static-wrappers.md`) automatically after every install, using two
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

## Why both are needed (the gap that breaks if only one exists)

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
  for `$NEWPREFIX/bin` (see `design-install-path.md`).
- Neither alone is sufficient: apt-driven installs go through both apt
  *and* the dpkg it calls internally, so the dpkg wrapper's own hook logic
  needs a guard against double-running (an env var set by the apt hook's
  caller, or a lock/marker file for "already ran for this transaction") —
  otherwise a single `apt-get install` would trigger wrapper generation
  twice, redundant but not obviously wrong, so worth avoiding cleanly
  rather than leaving as a known-harmless quirk.

## Config sketch

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

## What "wrap-new-binaries" needs to know

To avoid re-scanning every binary in the prefix on every invoke, it needs
to know what changed since the last run — sudo-less's `prefix-wrap` scopes
itself to "each package installed or changed since the last run" via a
stamp file. Same approach here: compare `$NEWPREFIX/var/lib/dpkg/status`
mtime (or a package-list diff) against a stamp under
`$NEWPREFIX/.deb-native/wrap.stamp`, matching sudo-less's
`$PREFIX/.sudo-less/view/mirror.stamp` pattern referenced in `view.md`.

## Open work

- [ ] Confirm `DPkg::Post-Invoke` fires reliably when apt's `Dir::*` is
      pointed at `$NEWPREFIX` via the custom `apt.conf` from
      `design-install-path.md` — not yet tested on-device.
- [ ] Decide the double-run guard mechanism (env var vs. lock file vs.
      transaction id) once the wrapper script itself exists to test
      against.
- [ ] Decide where the real dpkg binary lives once wrapped (can't keep the
      name `dpkg` in the same directory as the wrapper).
