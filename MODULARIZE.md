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
    `dn-shim.so`, `dn-run`, `dn-trace`, `dn-elf`, and the
    hook/launcher scripts. Pure gcc against the prefix's glibc; no glibc source,
    no cross-toolchain.
  - **B3 — assemble**: seed B1's files, install B1's debs, place B2's overlay,
    install the base → a prefix.
  - **B4 — package**: `build/package-prefix.sh` → tarball + manifest.

  Only **B1** is genuinely heavy and version-critical; **B2** is an ordinary
  gcc build. **Every bootstrap stage / chicken-and-egg problem lives here and
  only here.** Environment: CI or a gcc-capable host.
- **Ship** delivers a prebuilt artifact to a target and runs it, with **no
  toolchain and no building**. There is **exactly one ship path**, the same on
  every target (`docs/spec/prefix-contract.md`): read the artifact's
  `.dn/contract`, check it, extract, and run its relocation script when it
  landed elsewhere than its build path.
  - `install.sh` (Termux) — obtain the artifact, then the one ship path. It
    never builds: with no artifact for its destination it stops and says so.
    The whole of build -- toolchain -> components -> assembled prefix ->
    tarball -- is a separate stage, run wherever a toolchain is (CI, or this
    device as a build host), and its only output is the tarball.
  - the `dn-shell` app — the one ship path on the bundled asset
    (`dn-prefix install`).
- A **target never builds**; a **builder never needs the target**. The execution
  layer (`core/`) and the adapter are the same on both sides.

### Ship is the host's shell only

Shipping runs entirely in the shell the target already has (app:
`/system/bin/sh` + toybox; Termux: the Termux shell), with the same steps on
every target (`docs/spec/prefix-contract.md`): read `.dn/contract` without
extracting, check it, extract, and -- only when the prefix landed somewhere
other than its build path -- run the prefix's own `.dn/relocate.sh` with that
same shell. The relocation script is the prefix's logic: it overwrites each
`PT_INTERP` string in place with `dd`, at the offset and within the capacity
the build recorded in `.dn/baked-paths`, and rewrites text with `sed -i`. No
ELF tool and no program of the prefix runs during an install; the host never
edits a file inside a prefix. Nothing else
runs at install: what depends on the running system belongs to the prefix's
`boot.d`/`login.d` hooks.

**Goal: ship the smallest artifact.** Build trims it (`build/trim-prefix.sh`)
and packages it (`build/package-prefix.sh`); the floor is `apt`+`dpkg`+`bash`
plus their dependency closure, `glibc`, and the deb-native overlay.

### The minimal prefix and the layers above it

Two prefixes are built (`docs/spec/prefix-layers.md`):

- **core-ultra** -- the minimal prefix, done when its shell runs: the
  patched glibc, the shim, `dn-run`, `dn-shell`, `bash`/`dash`, basic tools,
  `patchelf`, CA certificates, and `.dn/packages` (the Debian packages it
  contains). The blueprint for specialized prefixes.
- **core-deb** -- the core-ultra recipe plus the Debian layer: `apt`,
  `dpkg` and their closure (`libapt-pkg`, `libstdc++`, `gpgv`, compression
  libraries, ...), the translation hooks, `priv/`, the launchers, the
  maintainer-script interpreters and essentials (`debconf`/`cdebconf`, `xz`,
  `debian-archive-keyring`, ...). The floor for `apt install` (never trimmed
  below, or a later install breaks).

The layering is build-time only: each is its own artifact, installed once and
complete; nothing is added into an installed prefix as a module.

### Post-build pipelines (after the artifact exists)

In order, and each owned by one place:

1. **Deploy** — obtain the artifact, read and check `.dn/contract`, extract,
   run `.dn/relocate.sh` when the prefix landed elsewhere than its build path.
   Owner: the target's host shell (`/system/bin/sh`+toybox, or the Termux
   shell); the steps are identical.
2. **Finish (build side)** — `core/runtime/dn-finish.sh`'s steps (patched
   glibc, symlinks, alternatives, launchers) run at build time, before
   packaging; none is needed at install (`docs/spec/prefix-contract.md`,
   "Build invariants").
3. **Package install (runtime)** — `apt install` → the prefix's hooks
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
- [x] Move `core/native/`, `core/tracer/`, `core/custom/` -> `core/` (compat symlinks left at
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
      adapter; `module-edges.allow` is empty. (All three retired since: the
      overlay is glibc-only and prebuilt (`build/build-overlay-glibc.sh`), the
      prefix ships as a `.dn/` artifact, and the target's install is
      `core/runtime/ship-prefix.sh` + the artifact's own `.dn/install.sh`.)
- [x] Move the adapter leaf scripts to `adapters/deb-native/`
      (`make-shell-interface.sh`, `make-shell-interface.sh`).
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
- **Install interface:** a `.dn/` directory at the root -- the contract a host
  reads without running code, the map of every byte range that names the
  build path, and a relocation script the host's own shell runs
  (`docs/spec/prefix-contract.md`, design, not yet built). The artifact is
  not relocated at build time; one artifact installs at any path.
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
| `core/native/dn-launch.c` vs fork `dn-shell.c`/`dn-perl.c` | same invariant, different interpreter choice | **adapter parameter**, not a split |
