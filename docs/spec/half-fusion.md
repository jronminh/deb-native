# Half-fusion — a middle mode between the prefix and `naibed`

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing one,
> when the content is a distinct kind of writing — a new spec topic, a
> new one-off investigation, or a new guide — not just a long addition
> to what a doc already covers.

**Status: skeleton / idea only — no code on this branch yet.** A third
deployment mode for deb-native, between `main`'s sealed prefix and
`naibed`'s true fusion: keep the two trees separate, but **selectively and
reversibly expose** parts of the Debian prefix **into** Termux, and share
**one `$HOME`** between them. Reuses `main`'s loader and shim unchanged;
this is an integration layer *above* them, not a rewrite.

## Contents

- [The middle point](#the-middle-point)
- [The leak axes](#the-leak-axes)
- [Decision 1: collisions warn and abort](#decision-1-collisions-warn-and-abort)
- [Decision 2: one shared HOME](#decision-2-one-shared-home)
- [Invariants](#invariants)
- [Planned pieces (no code yet)](#planned-pieces-no-code-yet)
- [Milestones](#milestones)
- [Open questions](#open-questions)

## Related docs

- [`design.md`](design.md) — `main`'s self-contained prefix, the baseline.
- [`path-shim.md`](path-shim.md) and [`ld-dn-config.md`](ld-dn-config.md) —
  the loader/shim machinery this reuses; its `shim-prefix` /
  `DN_REDIRECT_PREFIXES` knob is the main enabler.
- [`multiarch-mechanics.md`](multiarch-mechanics.md) — the multi-arch
  framing used for name collisions.
- [`runtime-failures.md`](runtime-failures.md) — the shared-`$HOME`
  collisions this mode has to manage.
- The `naibed` branch — the true-fusion extreme this is deliberately not.

## The middle point

Three points on one line:

| | tree | Termux sees | reversible |
|---|---|---|---|
| **`main`** | separate `~/.dn` | almost nothing (only by-name via PATH) | yes, delete `~/.dn` |
| **half-fusion** | separate `~/.dn` | a **controlled, opt-in view** of it | yes, additive symlinks + manifest |
| **`naibed`** | Termux's `$PREFIX` **is** Debian | everything | **no** (one-way) |

Half-fusion is `main` plus an exposure layer: same prefix, same
`ld-dn`/shim, but you choose which Debian bits surface in Termux, and
undoing is exact. `naibed` replaces Termux; this **overlays** it — Termux
stays authoritative.

## The leak axes

| Axis | Meaning | Verdict |
|---|---|---|
| **A. Commands** | Debian programs on Termux's `PATH` / as symlinks | safe, high value |
| **B. Tree** | `$PREFIX/opt/dn -> ~/.dn`, a read-only view | safe |
| **C. Config/data** | `resolv.conf`, CA certs, timezone, XDG, `$HOME` | per-item; `$HOME` is the flagship (Decision 2) |
| **D. Package manager** | `apt`/`dpkg` naming, a unified front end | powerful; sharing the dpkg DB is fusion-lite, not this |
| **E. Libraries** | Debian libs visible to a Bionic process | **never** — the hard line |

Direction is one-way and additive: Debian → Termux. Nothing Termux owns
is modified.

## Decision 1: collisions warn and abort

When an exposed name already exists in Termux, do **not** silently pick.
Default: print a report, then **abort the whole expose step**:

```
name      termux (bionic)         debian (glibc)
python    python3.11              python3.13
openssl   OpenSSL 3.3             OpenSSL 3.0
```

Treat the two as co-installable **architectures of the same name** —
`name@termux` and `name@debian`, the dpkg-multiarch framing (see
[`multiarch-mechanics.md`](multiarch-mechanics.md)). The only question is
which one `PATH` shows.

Resolution inputs (the "multiarch preference" file):
- `collisions.toml`: `[policy].default = abort | termux | debian | qualify`
  and per-name `[prefer] python = "debian"`.
- CLI flags `--on-collision=…`, `--partial` (expose the rest, skip the
  colliding names), `--resolve name=side`.
- `qualify`: inert names get `$PREFIX/bin` symlinks; colliding ones stay
  reachable only as `$PREFIX/opt/dn/bin/<name>` (or a `dn-<name>` alias) —
  true coexistence, no precedence decision.
- `dn which <name>` shows both variants and the winner.

Because nothing is overwritten, the policy choice never affects
reversibility.

## Decision 2: one shared HOME

Termux's `$HOME` is **the single home for both systems** — dotfiles,
`~/Documents`, `~/Downloads`, media, and caches are literally shared. This
is the flagship "leak". Wiring (mostly already implied by `main`): Debian
programs inherit `HOME` from the Termux shell, and the prefix's `/root`
stays a symlink to the shared home; `dn-shell` starts there; the prefix
never gets a real `/root` directory of its own.

Guardrail for the one real hazard — two libcs writing the same subtrees
([`runtime-failures.md`](runtime-failures.md)):

```
[home]
shared = true
# split only these by libc, under the one home; everything else is shared
namespace = ["~/.local/lib", "~/.local/bin",
             "~/.cache/pip", "~/.cache/uv", "~/.cargo", "~/.rustup"]
rc = "shared"   # shared | split
```

Implementation intent: `$HOME` itself is identical; the ABI-bound dirs are
redirected for Debian programs only (`PYTHONUSERBASE`, `XDG_CACHE_HOME`,
`XDG_DATA_HOME`, `CARGO_HOME`) into a libc-tagged subtree under that same
home. The user experiences one home; the incompatible caches stay apart.

## Invariants

1. **Never overwrite a Termux-owned path.** New names only; the manifest
   proves it.
2. **Never expose `$DN/usr/lib` to a Bionic process.** Debian libs load
   only inside Debian programs (the shim already strips inheritance for
   Bionic children — keep it).
3. **Reversible by construction.** `dn-unfuse` plus a manifest restores
   `$PREFIX` to byte-identical.
4. **Regenerate on change.** A hook on install/refresh keeps the exposure
   in step with the package set.

## Planned pieces (no code yet)

```
scripts/integrate/
  expose-commands.sh   # symlink farm into $PREFIX/opt/dn/bin, collision policy
  expose-tree.sh       # $PREFIX/opt/dn -> ~/.dn
  sync-config.sh       # opt-in resolv.conf / CA / timezone / XDG wiring
  dn-unfuse.sh         # remove exactly what was added (manifest-driven)
  collisions.toml      # [policy] default, [prefer] per-name
  home.toml            # [home] shared, namespace, rc
  manifest             # generated list of created links/files
```

`dn expose` / `dn unfuse` front the scripts; a `dn which` reports
collisions. See [`scripts/integrate/README.md`](../../scripts/integrate/README.md).

## Milestones

- **M1** — `dn expose` with collision report + abort, `collisions.toml`,
  `qualify`/`--partial`, and `dn-unfuse` manifest round-trip (`$PREFIX`
  byte-identical after undo).
- **M2** — shared `HOME` wiring + per-libc namespace list; a Debian and a
  Termux `python3` must not share `~/.local/lib`, but must see the same
  `~/Documents`.
- **M3** — `dn which`, drift report after package changes, and the
  package-manager front-end decision (still additive).

## Open questions

- Which C-leaks beyond `HOME`: `resolv.conf`, CA bundle, timezone, XDG —
  and where does sync stop?
- `rc = shared` vs `split`: a Debian shell reading Termux's `~/.bashrc`
  can hit Termux-only paths — guard or split?
- `~/.local/bin`: shared (one user CLI namespace) or namespaced (each libc
  gets its own), given both install user commands there?
- Collision default for a first run: hard abort, or `--partial` with a
  loud summary?
- Does anything here want an `ld-dn.conf` `[program]` block, or is the
  exposure layer entirely independent of the loader?
