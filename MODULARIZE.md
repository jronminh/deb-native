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

## Phases

### P0 — Map and rules
- [x] Assign every tracked file to exactly one module —
      `scripts/tools/module-map.tsv`.
- [x] Write the check — `scripts/tools/check-modules.py`: one home per file, and
      no `core` file references `build`/`bootstrap`/`adapter`/`product`.
      Accepted edges live in `scripts/tools/module-edges.allow` (2 entries, to
      burn down in P2).
- [ ] Land the check: wire it into `.github/workflows/checks.yml` (P4).

### P1 — Move-only restructure
- [ ] Create top-level `core/ build/ bootstrap/ adapters/<target>/ product/
      tests/ docs/`.
- [ ] Move files per the P0 map. Leave **compat symlinks** at old paths so
      nothing breaks.
- [ ] No logic changes in P1.

### P2 — Split the straddlers, parameterize the leaks
- [ ] Split `scripts/install/setup-runtime.sh` into `build-core`,
      `install-runtime` (prefix install + `priv/`), and `vendor-host-libs`
      (adapter).
- [ ] Move `scripts/runtime/make-shell-interface.sh` to the adapter module.
- [ ] Parameterize adapter leaks that live in core files: which `dpkg-query`
      (host vs prefix), the `$HERE/../../custom` path, `BASE_FILES`, and the
      interpreter choice.
- [ ] Give `install-hooks.sh` a **core manifest** so it stops copying
      bootstrap scripts.

### P3 — Bind interfaces and pin versions
- [ ] Name and version the prefix artifact and the core entry points.
- [ ] Add a **smoke test** that runs the core test suite against both adapter
      instantiations (fixed prefix and dynamic prefix).

### P4 — Enforce
- [ ] Wire the check script and the smoke test into the normal loop (CI or
      local).
- [ ] "One home per file" becomes machine-checked, not a convention.

### P5 — Extract repos (only if needed)
- [ ] `core` (shared), `bootstrap` (or keep as subtree), `adapter`.
- [ ] `dn-shell` consumes the **pinned prefix artifact**, never bootstrap code.

## Decisions to settle before P1

1. **Which model is canonical** — fold the fork's model (dynamic/relocatable
   prefix, vendored glibc, `dn-shim`, split glibc interpreters) **into core**,
   or make the fork the new core and have `deb-native` track it?
2. **Bootstrap stays a subtree** in-repo, or splits out? (Recommendation:
   subtree first.)
3. **Interpreter choice** (`dn-launch.c` Bionic single binary vs
   `dn-shell.c`/`dn-perl.c` glibc) — settle it as an **adapter parameter**.
4. **Disambiguate the name `dn-shell`** — it currently means three things: the
   maintainer-script interpreter, the generated userland-entry script, and the
   app. Core must keep clean names.

## Warnings

- Splitting into too many repos *is* operational scatter. Subtree + enforced
  boundary first; extract only on real need.
- Version skew between core and the prefix artifact is the number-one risk:
  pin, and smoke-test — no exceptions.
- `docs/` keeps only current truth; this plan and the roadmap stay outside it.

## Appendix A — Current straddlers and their verdicts

| File | Mixes | Verdict |
|---|---|---|
| `scripts/install/setup-runtime.sh` | build core artifacts; install into prefix + generate `priv/`; vendor host libs + rpath | **split** into build / install / vendor |
| `scripts/runtime/make-shell-interface.sh` | pure adapter (login, userland switch, motd, `pkg` guard, prefix registry) | **move** to adapter |
| `scripts/install/dn-hook-pre.sh` | core hook; collision check calls the host `dpkg-query` | **parameterize** |
| `scripts/install/dn-hook-post.sh` | core hook; host paths | **parameterize** |
| `scripts/runtime/make-launchers.sh` | core logic; host `dpkg-query` + `BASE_FILES` | **parameterize** |
| `scripts/runtime/make-apt-wrappers.sh` | host package-manager wrappers | **move/parameterize** |
| `scripts/runtime/install-hooks.sh` | core staging; copies bootstrap scripts too | **core manifest** |
| `scripts/install/dn-translate-deb.sh` | core; relative `$HERE/../../custom` layout assumption | **parameterize** |
| `scripts/bootstrap/build-path-redirect.sh` | build recipe for a core source, using the bootstrap toolchain | **keep** (bootstrap builds a core source) |
| `install.sh` | bootstrap/product entry point that calls core | **keep** (product) |
| `native/dn-launch.c` vs fork `dn-shell.c`/`dn-perl.c` | same invariant, different interpreter choice | **adapter parameter**, not a split |
