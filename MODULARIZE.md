# MODULARIZE.md — modularizing the deb-native codebase

Working plan, deliberately separate from `TODO.md` (which stays the product
roadmap). This file is about **structure**: splitting `deb-native` into a shared
core, a build layer, a bootstrap layer, and thin per-target adapters, so the
`dn-shell` fork and the on-device product stop being two hand-maintained copies.

Rule of this file: modularize **first**, extract repos **later**. Enforce the
boundaries inside one repo; only split a repo once the boundary has proven
stable and a real consumer needs it.

## Principles

1. **One-way dependencies.** `core` must not know `build`, `bootstrap`, or
   `adapter`. Any edge pointing back up is a file in the wrong home.
2. **Modularize before extracting.** Boundaries enforced in-repo have no
   version-skew surface; separate repos do.
3. **One home per file.** No shared file keeps two copies.
4. **Interfaces are named and pinned.** The bootstrap → target handoff is a
   versioned **prefix artifact**; the core exposes stable entry points.

## Module taxonomy

| Module | Contains | Depends on | Produces |
|---|---|---|---|
| **core** | package-translation logic, path shim, syscall tracer, launcher/classifier, loader config, the `priv/` privilege mechanism, invariant docs | — | stable entry scripts + libraries |
| **build** | recipes that compile core sources (shim, `dn-run`, tracer, interpreter binaries) | core (sources) | built artifacts |
| **bootstrap** | acquire/patch glibc, base packages, apt/dpkg setup, index rewrite, one-shot pipeline | core + build | a **pinned prefix artifact** |
| **adapter** | prefix derivation (fixed vs dynamic), interpreter choice, `PATH`/userland/shell, app packaging, any host borrowing | core | per-target glue |
| **product** | `install.sh` / APK packaging; binds bootstrap-or-artifact to one target | bootstrap \| artifact + adapter | an installable product |
| **tests** | harness | core + build | pass/fail |
| **docs** | invariants (core) + per-target docs | — | — |

Dependency direction (arrows point only downward, no cycles):

```
        ┌────────┐
        │  core  │◄────────────┐
        └───┬────┘             │
   source   │                  │ source
        ┌───▼────┐         ┌───┴─────┐
        │ build  │         │ adapter │
        └───┬────┘         └───┬─────┘
   artifacts│                  │ glue
        ┌───▼───────┐          │
        │ bootstrap │─artifact─┤
        └───┬───────┘          │
            │                  ▼
            │            ┌──────────┐
            └───────────►│ product  │
                         └──────────┘
```

## Module interfaces

- **core** exposes stable **entry scripts** (`dn-translate-deb`, `dn-hook-pre`,
  `dn-hook-post`, `make-launchers`, `dn-run`, the shim, the tracer) and
  publishes the list of **artifacts** build must produce.
- **build** exposes artifacts with names/versions; holds no core logic.
- **bootstrap** exposes a **pinned prefix artifact** — the clean boundary every
  target consumes (this is exactly the `dn-shell` APK's bundled prefix).
- **adapter** exposes a small **parameter set**: prefix derivation, interpreter
  choice, `PATH` order, packaging paths.

## Build vs Ship: two independent concerns

Deliberately not mixed — this is what removes the chicken-and-egg from every
target.

- **Build** produces the prefix artifact
  (`deb-native-prefix-<version>-<arch>.tar.gz` + manifest) and splits by weight:
  - **B1 — glibc** (heavy): build the project's own glibc into `libc6.deb` +
    `libc-bin.deb` and the patched glibc files (the "10-file swap", from
    `third_party/glibc-android-patches`). That bundle **is** `DN_GLIBC_DEBS`.
  - **B2 — overlay** (light, plain gcc): build the runtime overlay —
    `dn-shim.so`, `dn-run`, the interpreters `dn-sh`/`dn-perl`, and the
    hook/launcher scripts. Pure gcc against the prefix's glibc; no glibc source,
    no cross-toolchain.
  - **B3 — assemble**: seed B1's files, install B1's debs, place B2's overlay,
    install the base → a prefix.
  - **B4 — package**: `build/package-prefix.sh` → tarball + manifest.

  Only **B1** is genuinely heavy and version-critical; **B2** is an ordinary
  gcc build. **Every bootstrap stage / chicken-and-egg problem lives here and
  only here.** Environment: CI or a gcc-capable host.
- **Ship** delivers a prebuilt artifact to a target and runs it, with **no
  toolchain and no building**. Consumers do the same thing: obtain the artifact,
  extract it, fix up, run.
  - `install.sh` (Termux) — download/extract the artifact, then `apt update`.
  - the `dn-shell` app — extract the bundled asset on first run.
- A **target never builds**; a **builder never needs the target**. The execution
  layer (`core/`) and the adapter are the same on both sides.

### Ship is two phases; only the first is target-specific

- **Phase A — target-native trigger**: run the shell the target already has.
  - app: `/system/bin/sh` + toybox (`tar xzf`) — always present, no glibc.
  - Termux: the Termux prefix shell (bash/dash + coreutils).
  Job: extract the artifact into the target location (plus relocation, only if
  the prefix is not fully relocatable).
- **Phase B — prefix-native finish**: once the prefix runs, use the prefix's own
  shell to refresh the loader cache, normalize symlinks, and `apt update`. This
  logic is identical for every target and lives in `core/`.

Only the Phase-A trigger differs per target, and it lives in the adapter. A
fully relocatable prefix (loader/shim self-derive; no baked absolute paths)
reduces Phase A to `tar xzf` — one line naming the target's shell.

**Goal: ship the smallest artifact.** Build trims it (`build/trim-prefix.sh`)
and packages it (`build/package-prefix.sh`); the floor is `apt`+`dpkg`+`bash`
plus their dependency closure, `glibc`, and the deb-native overlay.

### The minimal prefix (the shipped floor)

Enough to boot `apt` and let the prefix expand itself; nothing more:

- **deb-native overlay** (`core/`): `dn-shim.so`, `dn-run`, `dn-trace`, the
  interpreters, the hook/launcher scripts, `priv/`.
- **`apt`, `dpkg`, `bash`**, and **`patchelf`** — patchelf is required by
  `dn-translate-deb.sh` on every runtime install (~0.3 MB; its deps are
  already present).
- **`glibc`** (`libc6` + `libc-bin`).
- **The dependency closure** of the above: `libapt-pkg`, `libstdc++`, a crypto
  library plus `gpgv`, `zlib`, `liblzma`/`libbz2`/`libzstd`, `libtinfo`/
  `libreadline`, ...
- **Maintainer-script essentials**: `coreutils`, `sed`, `grep`, `mawk`, `tar`,
  `gzip`, `xz`, `dash`, `debconf`/`cdebconf`, `base-files`, `base-passwd`,
  `debianutils`, `debian-archive-keyring`, `ca-certificates`.

Everything else is trimmed; the floor above is never crossed (or a later
`apt install` breaks).

### Post-build pipelines (after the artifact exists)

In order, and each owned by one place:

1. **Deploy (Phase A, target-native)** — obtain the artifact, extract to the
   target path, relocate if it was not built for that exact path. Owner: the
   target adapter (`system/bin/sh`+toybox, or the Termux shell).
2. **Finish (Phase B, prefix-native)** — `core/runtime/dn-finish.sh`: restore the
   patched glibc, refresh the loader cache, normalize symlinks, fix alternatives,
   regenerate launchers, optionally `apt update`. Identical for every target.
3. **Install (runtime)** — `apt install` → the prefix's hooks
   (`dn-hook-pre` → `dn-translate-deb`; `dn-hook-post` → glibc-swap guard,
   alternatives, symlinks, launchers, gcc specs).
4. **Update** — `dn-update` for the overlay (per the manifest), `apt upgrade`
   for the base; the post-invoke glibc-swap guard covers the latter.
5. **Run** — launcher → `dn-run` classifies the ELF → prefix loader + shim, or
   the tracer, or adoption.

## Phases

### P0 — Map and rules
- [x] Assign every tracked file to exactly one module —
      `tools/module-map.tsv`.
- [x] Write the check — `tools/check-modules.py`: one home per file, and
      no `core` file references `build`/`bootstrap`/`adapter`/`product`.
      Accepted edges live in `tools/module-edges.allow` (2 entries, to
      burn down in P2).
- [ ] Land the check: wire it into `.github/workflows/checks.yml` (P4).

### P1 — Move-only restructure (done)
- [x] Move `native/`, `tracer/`, `custom/` -> `core/` (compat symlinks left at
      the old names).
- [x] Move `scripts/**` -> `core/install`, `core/runtime`, `core/bench`,
      `build/`, `bootstrap/`, `tools/`; `install.sh` stays at the repo root
      (product) so the documented `curl | sh` URL is unchanged.
- [x] No logic changes; `check-paths.py` guards the `$HERE` references.
- [ ] Drop the `native`/`tracer`/`custom` compat symlinks (P5).

### P2 — Split the straddlers, parameterize the leaks
- [x] Split `setup-runtime.sh` into `build-core` (build), `install-runtime`
      (core: install + `priv/`), and `vendor-host-libs` (adapter). The
      orchestrator stays bootstrap-level, so `core` no longer reaches build or
      adapter; `module-edges.allow` is empty.
- [x] Move the adapter leaf scripts to `adapters/deb-native/`
      (`make-shell-interface.sh`, `make-apt-wrappers.sh`).
- [x] Parameterize the `dpkg-query` leak (prefer the prefix's own; borrow the
      host's only during bootstrap) in `dn-hook-pre.sh` and `make-launchers.sh`.
- [x] `install-hooks.sh` copies an explicit core manifest, not `*.sh`.
- [ ] Still open: `dn-translate-deb.sh`'s `$HERE/../../custom` path and the
      interpreter choice (`dn-launch.c` vs the fork's glibc binaries) are not
      yet parameters.

### P3 — Bind interfaces and pin versions (in progress)
- [x] Declare the core surface in `core/interface.tsv` (entries, sources,
      artifacts) and pin the version in `core/VERSION`; enforced by
      `tools/check-interface.py` (non-core modules may call only the
      declared entries).
- [ ] Define the **prefix artifact** naming/versioning rule from
      `core/VERSION` (the artifact bootstrap produces and a target consumes).
- [ ] Add a **smoke test** that runs the core test suite against both adapter
      instantiations (fixed prefix and dynamic prefix) — blocked on a second
      adapter (the dn-shell fork) existing in-tree.

### P4 — Enforce
- [ ] Wire the check script and the smoke test into the normal loop (CI or
      local).
- [ ] "One home per file" becomes machine-checked, not a convention.

### P5 — Extract repos (only if needed)
- [ ] `core` (shared), `bootstrap` (or keep as subtree), `adapter`.
- [ ] `dn-shell` consumes the **pinned prefix artifact**, never bootstrap code.

## Decisions (resolved 2026-10-05)

1. **Canonical model** — the **fork's** model: dynamic/relocatable prefix,
   vendored glibc, `dn-shim`, split glibc interpreters (`dn-shell`/`dn-perl`).
   `deb-native`'s fixed-prefix, Termux-backed model is the older variant.
   *Blocker:* the fork's C sources (`dn-shim.c`, `dn-shell.c`, `dn-perl.c`,
   `dn-child.h`) are not in this tree or any branch — they must be supplied
   before core can adopt this model.
2. **Bootstrap** stays an in-repo subtree; extract a repo only if needed (P5).
3. **Interpreter choice** is an **adapter parameter**; the glibc interpreters
   replace the Bionic `dn-launch.c` when the fork model lands.
4. **`dn-shell` disambiguation** — the **app/repo** keeps `dn-shell`; every
   other `dn-shell` in the code is renamed to another `dn-*` name:
   - maintainer-script interpreter binary `usr/bin/dn-shell` -> `usr/bin/dn-sh`
     (sibling `dn-perl` keeps its name);
   - adapter userland-entry wrapper `$TP/bin/dn-login` -> `dn-login`
     (matching `~/.dn-login`).
   A dedicated, context-aware rename (the string `dn-shell` also names the app,
   so a blind replace is unsafe). The shim rename (`path-redirect` ->
   `dn-shim`, matching the fork) is done.

### Prefix artifact (decided)

The bootstrap's output — the one boundary a target consumes:

- **Name:** `deb-native-prefix-<version>-<arch>.tar.gz`, `<version>` read from
  `core/VERSION`, `<arch>` = `arm64`.
- **Contents:** the prefix tree (`usr/`, `etc/`, `var/`, `opt/`) plus a manifest
  at `var/lib/deb-native/prefix-manifest.tsv` listing each component and its
  sha256, so `dn-update` can verify an overlay component.
- **Consumer:** a target adapter (e.g. `dn-shell`) packages the tarball as an
  app asset and pins the version; it never runs bootstrap code (P5).

## Warnings

- Splitting into too many repos *is* operational scatter. Subtree + enforced
  boundary first; extract only on real need.
- Version skew between core and the prefix artifact is the number-one risk:
  pin, and smoke-test — no exceptions.
- `docs/` keeps only current truth; this plan and the roadmap stay outside it.

## Appendix A — Current straddlers and their verdicts

| File | Mixes | Verdict |
|---|---|---|
| `core/install/setup-runtime.sh` | build core artifacts; install into prefix + generate `priv/`; vendor host libs + rpath | **split** into build / install / vendor |
| `core/runtime/make-shell-interface.sh` | pure adapter (login, userland switch, motd, `pkg` guard, prefix registry) | **move** to adapter |
| `core/install/dn-hook-pre.sh` | core hook; collision check calls the host `dpkg-query` | **parameterize** |
| `core/install/dn-hook-post.sh` | core hook; host paths | **parameterize** |
| `core/runtime/make-launchers.sh` | core logic; host `dpkg-query` + `BASE_FILES` | **parameterize** |
| `core/runtime/make-apt-wrappers.sh` | host package-manager wrappers | **move/parameterize** |
| `core/runtime/install-hooks.sh` | core staging; copies bootstrap scripts too | **core manifest** |
| `core/install/dn-translate-deb.sh` | core; relative `$HERE/../../custom` layout assumption | **parameterize** |
| `bootstrap/build-dn-shim.sh` | build recipe for a core source, using the bootstrap toolchain | **keep** (bootstrap builds a core source) |
| `install.sh` | bootstrap/product entry point that calls core | **keep** (product) |
| `native/dn-launch.c` vs fork `dn-shell.c`/`dn-perl.c` | same invariant, different interpreter choice | **adapter parameter**, not a split |
