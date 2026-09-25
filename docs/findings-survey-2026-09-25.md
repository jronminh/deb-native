# Findings: first random-sample survey (2026-09-25)

Following sudo-less's own methodology (`docs/survey-2026-09.md`,
`dev/survey.sh`): a random, reproducible sample of real Debian packages,
each installed into a fresh isolated prefix. `scripts/sample-packages.py`
(seed `20260925`, ≤2 per section, ≤5MB, excluding required/important/
standard-priority and metapackages) + `scripts/survey.sh`.

## Result: 2 of 30 installed (≈7%)

| install outcome | count |
|---|---|
| `ok` | 2 |
| `depmissing` | 27 |
| `unpack` | 1 |

Far below sudo-less's own 63% (`docs/survey-2026-09.md`), and far below
the "even 40% would be great" bar set going in. Both is honest data, not a
discouraging surprise once the cause is clear — see below.

## The real cause: this project has no dependency installer, at all

27 of 30 failures are `depmissing`: dpkg refuses to configure the package
because a dependency isn't present. This is **not** the native-seed
mapping gap `design-native-deps.md` flagged as open work (a ~10-entry
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
`native-seed.sh`'s small stub table covers. `design-install-path.md`
always intended real `apt` (not bare `dpkg`) for exactly this reason — but
that plan was never implemented or tested; every prototype and survey run
so far has used bare `dpkg` on a single file.

**This is the actual #1 priority now**, well ahead of anything else open
in this repo's docs (soname-based native mapping, generalizing the
`LD_PRELOAD` shim, etc.) — none of that matters if a package's own
ordinary dependencies were never fetched in the first place.

## The one `unpack` failure: not chased further

`libxmlezout-dev` failed on `Permission denied` unpacking a 35-character
filename — not a path-length issue (checked: well under any real limit).
Not investigated further; the prototype is still too early-stage for a
single odd failure like this to be worth a deep dive yet (could be the
survey's own very deeply-nested scratch `OUT` directory path, could be
something else — unknown).

## What this changes

- `design-native-deps.md`'s "Open work" (soname mapping, unverified table
  entries) is still valid work, but it's downstream of a much bigger gap:
  without real dependency resolution, most real packages never get far
  enough to need it.
- The right next step is wiring actual `apt` against a real prefix-scoped
  `apt.conf` (as `design-install-path.md` originally described but this
  repo never actually built) so ordinary dependencies get fetched and
  installed automatically — `native-seed.sh`'s stub table then only needs
  to cover the specific case it was built for (a dependency Termux's
  glibc side-install already provides, which should NOT be re-fetched),
  not stand in for a missing installer entirely.
- Re-run this same survey (same seed, same script) once real `apt`
  dependency resolution exists, to get a real before/after comparison.

## Raw data

`scripts/survey.sh` + `scripts/sample-packages.py` are committed and
reproducible (same seed `20260925`) — re-running produces the same 30
packages. Full per-package log kept locally during this run
(`OUT/logs/*.log`), not committed (large, single-run artifact).
