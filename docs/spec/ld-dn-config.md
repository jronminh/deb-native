# ld-dn config — a runtime-extensible loader policy

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing -- a new spec
> topic, a new one-off investigation, or a new guide -- not just a
> long addition to what a doc already covers.

Spec for `native/ld-dn.c`'s runtime policy: the preload libraries,
library search dirs, `COMPILER_PATH`, the env it sets and strips, the
loader it jumps to, and per-program overrides come from a small config
file read at exec time, not compiled into the binary. **Status:
implemented (0.5.2-prealpha).** The loader reads
`$DN/etc/deb-native/ld-dn.conf` (shipped as `native/ld-dn.conf`),
compiled defaults reproduce the pre-config behaviour when the file is
absent, and the shim reads `DN_REDIRECT_PREFIXES`. As-built specifics
and deviations are in [Status (0.5.2-prealpha)](#status-052-prealpha).
[`path-shim.md`](path-shim.md) and [`design.md`](design.md) describe the
surrounding mechanism.

## Contents

- [Why: the policy is compiled in](#why-the-policy-is-compiled-in)
- [What ld-dn does today](#what-ld-dn-does-today)
- [Constraints](#constraints)
- [Design options](#design-options)
- [Decision: a config file with compiled defaults](#decision-a-config-file-with-compiled-defaults)
- [Config file: location and format](#config-file-location-and-format)
- [Grammar](#grammar)
- [Processing order](#processing-order)
- [Per-program overrides](#per-program-overrides)
- [Feeding the shim and tracer](#feeding-the-shim-and-tracer)
- [Failure modes and safety](#failure-modes-and-safety)
- [Not configurable](#not-configurable)
- [Rebuild plan](#rebuild-plan)
- [Status (0.5.2-prealpha)](#status-052-prealpha)
- [Open questions](#open-questions)

## Related docs

- [`design.md`](design.md) — the live design; ld-dn's role in the
  prefix ("the loader stub").
- [`path-shim.md`](path-shim.md) — the `LD_PRELOAD` shim ld-dn installs,
  and the env its constructor reads.
- [`install-flow.md`](install-flow.md) — when `setup-runtime.sh` builds
  and copies ld-dn into a prefix.
- [`tracer.md`](tracer.md) — `dn-trace`, the other consumer of the
  policy (static binaries / raw syscalls).
- [`direct-usage.md`](direct-usage.md) — why a config-fed policy matters
  for the tracer's routing.

## Why: the policy is compiled in

ld-dn is the first code a Debian program runs: every translated binary
names it as `PT_INTERP` (`native/ld-dn.c:1`), so the kernel enters it
before glibc, before `main`, for **every process**. It is also the
earliest point that knows the prefix — it derives `$DN` from its own
path (`ld-dn.c:128-138`). That makes it the natural place to decide the
whole runtime's environment for the glibc process tree.

Today every one of those decisions is a C literal, so any change — one
more `LD_PRELOAD` library, a new multiarch libdir, a package that needs
its own env — requires editing `ld-dn.c` and recompiling it into every
prefix (`scripts/install/setup-runtime.sh:72-77`). The only runtime knob
is `DN_EXTRA_LIB_PATH` (`ld-dn.c:163-175`), added for exactly this
reason; it is a single-purpose escape hatch, not a policy.

The goal is not "no binary at all" — ld-dn must stay a freestanding
static ELF (it runs with no libc). The goal is that, after **one**
rebuild, growing its behaviour is a file edit in the prefix, not another
compile. This is the same shift `sudo-less` makes with its generated
`00local-prefix` config rather than per-case wrappers
([`classic-design.md`](classic-design.md)).

## What ld-dn does today

Ordered by what it touches (all in `native/ld-dn.c`):

| # | step | line |
|---|---|---|
| 1 | Derive `$DN` from the program's `PT_INTERP` suffix `/usr/lib/deb-native/ld-dn` | `128-138` |
| 2 | Read `LD_PRELOAD` (Termux's `termux-exec`) → save as `DN_BIONIC_PRELOAD`; read `DN_EXTRA_LIB_PATH` | `141-148` |
| 3 | Set `LD_PRELOAD` = `$DN/usr/lib/deb-native/path-redirect.so` | `149-150` |
| 4 | Set `DN_INSTDIR` = `$DN` | `151` |
| 5 | Set `LD_LIBRARY_PATH` = `$DN/usr/lib/aarch64-linux-gnu:$DN/usr/lib` + `DN_EXTRA_LIB_PATH` | `159-175` |
| 6 | Set `COMPILER_PATH` = `$DN/usr/bin` | `184-185` |
| 7 | Rebuild the stack, dropping `LD_PRELOAD`/`DN_INSTDIR`/`LD_LIBRARY_PATH`/`COMPILER_PATH`/`DN_BIONIC_PRELOAD` and appending the five above | `187-207` |
| 8 | Map `$DN/usr/lib/ld-linux-aarch64.so.1` by hand and enter it with `AT_BASE` | `209-256` |

Everything in steps 2–8 except `DN_INSTDIR` and the `PT_INTERP` suffix
is policy a config could own. The stack-rebuild arithmetic (`ld-dn.c:189`,
the `+5`) and the static string buffers (`ld-dn.c:107`) both assume the
entry count is known at compile time; a config that adds env entries
makes both dynamic.

Fixed limits that interact with config growth: 64 KiB of stack reserved
for the rebuilt vector (`RESERVE`, `ld-dn.c:73`, checked at `:190`) and
the `g_env_*` buffers (largest `8300` bytes, `ld-dn.c:107`). The runtime
audit (`TODO.md`, "Runtime component audit") measured roughly 8000 words
of headroom against realistic arg/env sizes, so a few KB of extra config
env is comfortably safe — but the caps must be stated, not assumed.

## Constraints

These rule out the easy implementations and shape the design:

- **Freestanding.** No libc, no `fopen`/`getenv`; raw syscalls only
  (`sys6`, `ld-dn.c:75`). A config reader is `openat` + `read` + `close`
  into a static buffer. The build is `-static -nostdlib -ffreestanding`
  (`ld-dn.c:21-24`).
- **Runs once per exec, on the hot path.** `find -exec`, `make -j`,
  a shell's command loop all exec thousands of times. An extra file read
  per exec is real; it must be cheap or gateable.
- **Bootstrap chicken-and-egg.** The prefix's `/etc` may not exist yet
  on the very first install. Compiled-in defaults must remain and must
  work with no config file present.
- **No dynamic allocation.** Static arena only; overflow must be a
  measured, warned, bounded condition.
- **Trust boundary unchanged.** The config lives inside the user-owned
  prefix and a `preload` directive runs code in every prefix process.
  That is no more authority than the env a user can already set, but the
  blast radius is "every Debian program silently stops working", so the
  reader must fail open, not closed.

## Design options

| option | where policy lives | rebuild needed to extend | verdict |
|---|---|---|---|
| **A. Env only** — grow the `DN_*` surface (`DN_PRELOAD`, `DN_ENV_*`, ...) | caller's environment | no, but not persistent and easy to contaminate | too weak alone |
| **B. File only** | `$DN/etc/deb-native/ld-dn.conf` | no | good, but no bootstrap default if compiled policy is *removed* |
| **C. Codegen at install** — generate a header, recompile | generated `.h` | **yes** | explicitly rejected by the goal |
| **D. Hybrid — compiled defaults + file override + env one-shot** | both | no | **chosen** |

Hybrid matches the idiom already in the shim: `dn_init()`
(`path-redirect.c:101-118`) reads `DN_INSTDIR`, `DN_REDIRECT_DEBUG`,
`DN_BIONIC_PRELOAD`, `DN_ID` and layers them over internal defaults.
ld-dn should do the same, one layer earlier.

## Decision: a config file with compiled defaults

Chosen design:

1. ld-dn keeps a **compiled-in default policy** (today's literals),
   so a prefix with no config behaves exactly as now.
2. If a config file is present (and, recommended, announced by a
   `DN_CONFIG` env var the launchers set — see below), ld-dn reads it
   and **overlays** it on the defaults.
3. `DN_EXTRA_LIB_PATH` (and a new `DN_PRELOAD`) remain **one-shot env
   overrides** on top, for a single command or a test.

The config file describes the *Debian view*; ld-dn, which already knows
`$DN`, is the one that turns it into real host paths. That keeps the
file portable across prefixes and is why this belongs in ld-dn and not
in the shim.

## Config file: location and format

Path: `$DN/etc/deb-native/ld-dn.conf`.

A checked-in default lives at `native/ld-dn.conf` and
`setup-runtime.sh` copies it to that path (alongside
`path-redirect.so`/`dn-run`/`ld-dn`, `setup-runtime.sh:39-77`). If the
file is missing, the compiled defaults apply — this is the bootstrap
case, and it is also what a minimal prefix that never edits config sees.

Line-oriented, UTF-8/ASCII, `#` comments, blank lines ignored. Small
by design; cap the read at a fixed size (proposed 16 KiB) and reject
anything larger rather than truncating mid-rule.

Two path conventions, chosen so the file stays prefix-agnostic:

- **`loader` / `lib-add` / `preload`** name files *inside the prefix*.
  A leading `/` is the Debian path; ld-dn joins it to `$DN`. So
  `/usr/lib` and `usr/lib` both mean `$DN/usr/lib`.
- **`env KEY=VALUE`** values are literal except the token `$DN`, which
  expands to the prefix (needed because `COMPILER_PATH` and friends must
  be real host paths for the glibc side).

`unset` and `shim-prefix` take names/guest-roots, never host paths.

## Grammar

```
# $DN/etc/deb-native/ld-dn.conf
# every directive is optional; omission keeps the compiled default

loader /usr/lib/ld-linux-aarch64.so.1      # default (ld-dn.c:210)

lib-add /usr/lib/aarch64-linux-gnu          # replaces the compiled two-dir
lib-add /usr/lib                            # default when both are given

preload /usr/lib/deb-native/path-redirect.so  # default; order is significant
# no-default-preload                         # suppress the built-in first entry

env     COMPILER_PATH=$DN/usr/bin            # set/replace (ld-dn.c:184)
env     LOCPATH=$DN/usr/lib/locale
unset   COMPILER_PATH                        # remove before appending
unset   UNWANTED_VAR

shim-prefix /usr /etc /var /opt /root /lib /bin /sbin   # to the shim, guest form

[program]                                   # per-program block (see below)
env     FOO=bar
preload /usr/lib/deb-native/foo.so
```

Semantics:

- `loader`, `lib-add`, `preload` are **ordered, repeatable**. `lib-add`
  is additive over the compiled list; an explicit `no-default-lib` (not
  shown above) would allow a fully caller-defined list if ever needed.
- The built-in `preload` (path-redirect.so) is always first unless
  `no-default-preload`; config `preload` lines append in file order,
  because a later library may depend on the path rewriting the first
  provides.
- `env` last-wins per key; `unset` removes an inherited or default key.
  The compiled strip list (`ld-dn.c:197`) becomes the default set of
  `unset` names (`LD_PRELOAD`, `DN_INSTDIR`, `LD_LIBRARY_PATH`,
  `COMPILER_PATH`, `DN_BIONIC_PRELOAD` minus those ld-dn re-adds).
- `shim-prefix` is passed *through* to the shim (next section), not
  consumed by ld-dn.

Deliberately **not** in v1: `include`, globs, arithmetic, conditionals.
A freestanding parser is the riskiest new code; keep its surface tiny so
it can be audited at a glance (see [`standard.md`](standard.md)).

## Processing order

On entry, in order:

1. Parse `envp`; find `DN_CONFIG` (path), `DN_EXTRA_LIB_PATH`,
   `DN_PRELOAD`, and the Termux `LD_PRELOAD`. Derive `$DN` from
   `PT_INTERP` as today (`ld-dn.c:128-138`) — this is unaffected by
   config.
2. Seed a `policy` struct from the compiled defaults.
3. If `DN_CONFIG` is set (or, if always-probe is chosen, unconditionally):
   `openat`+`read` the file into a static arena, parse, overlay. Any
   error is a warning to fd 2, not a `die`.
4. Overlay env one-shots: `DN_EXTRA_LIB_PATH` appends to the lib list,
   `DN_PRELOAD` appends to the preload list.
5. Emit: build env strings into the arena, count entries, rebuild the
   stack (now `words` computed from the policy, not the constant `+5`),
   map the chosen `loader`, enter it.

The prefix-relative rule is applied at emit time, so the parser never
needs to know `$DN` — it sees only the directive and its literal.

**Cost contract.** As built, ld-dn always probes
`$DN/etc/deb-native/ld-dn.conf` (one `openat` per exec on a page-cached
file, not a stat walk), so config works however a program is started,
including a by-name exec from a shell that was never activated. `DN_CONFIG`
points the loader at a different file; `DN_NO_CONFIG` skips the read
entirely (the cost-free path for a benchmark or a minimal prefix). The
gated alternative — only read when a launcher exported `DN_CONFIG` — was
considered and rejected for reachability. The per-exec `openat` still
wants a spawn-heavy benchmark (see the [Status](#status-052-prealpha)).

## Per-program overrides

A `[name]` block applies only when the executed program matches. ld-dn
can identify the program reliably: with no re-exec, `/proc/self/exe` is
the Debian program, not ld-dn (`ld-dn.c:16-18`), so a `readlink` gives
its path; the basename is the match key. Matching `basename` (e.g.
`[git]`) covers the common case; a value containing `/` matches the full
path suffix (e.g. `[/usr/bin/git]`). No globs in v1.

This is the payoff of the whole design: a package that needs an extra
`LD_PRELOAD`, a special `LOCPATH`, or a suppressed variable gets a
block in one file, in the prefix, with no rebuild and no change to
`dn-translate-deb.sh` or `custom/<package>.sh`.

Override semantics: a block's `env`/`preload` **add** to the global
policy for that program; a directive `reset` inside a block (future)
could make it replace instead. Keep v1 additive.

## Feeding the shim and tracer

ld-dn's config should be the one source for the rest of the runtime,
passed as env because that is how the shim reads it today
(`path-redirect.c:101-118`):

- `shim-prefix` → `DN_REDIRECT_PREFIXES` (colon/space list). The shim's
  redirect set is currently hardcoded to `/usr /etc /var /opt /root /lib
  /bin /sbin` (`path-redirect.c:143-165`); reading it from env removes
  the last case where adding a redirected root means editing C.
- `preload` lines are already env (`LD_PRELOAD`), so any shim-level
  extension rides along with no new plumbing.
- The tracer (`dn-trace`) receives `DN_INSTDIR` today
  (`dn-run.c:211`); config-driven bind roots would extend it the same way.

Doing this keeps a single config file as the policy for loader, shim and
tracer, and gives ld-dn a clean reason to exist beyond "set five vars".

## Failure modes and safety

- **Missing file** — compiled defaults; silent (or one debug line under
  `DN_REDIRECT_DEBUG`, the shim's existing debug flag).
- **Unreadable/oversized file** — warn to fd 2, use defaults, continue.
- **Malformed line** — warn with line number, skip that line, keep the
  rest (fail open; never `die` on config).
- **Unknown directive** — warn and skip, so a newer config on an older
  ld-dn degrades instead of bricking.
- **Arena overflow** — stop parsing, warn, keep what fit; the caps
  (`RESERVE`, arena size) are checked and reported, never silently
  truncating a terminated string.
- **Bad `loader`** — fall back to the compiled loader path and warn; a
  loader that fails to map is already fatal by nature (`ld-dn.c:212-227`)
  but should name the loader it tried, and where the path came from.
- **Security note** — `preload` = arbitrary code in every prefix
  process, and the prefix is user-writable. This grants no privilege the
  user lacks (env vars already do), but the doc and the reader should
  say plainly that the config is trusted input, and that a wrong
  `preload` affects every Debian program at once.

## Not configurable

Kept compiled-in, on purpose:

- The `PT_INTERP` suffix `/usr/lib/deb-native/ld-dn` — it is how ld-dn
  finds `$DN`, so it cannot depend on a file located via `$DN`.
- `DN_INSTDIR` — always the derived prefix; deriving it from config
  would let a file redirect every path in the process.
- The arm64 check (`e_machine == 183`, `ld-dn.c:215`) and the ELF
  mapping logic (`ld-dn.c:220-247`) — code, not policy.
- `RESERVE` and the arena caps — constants with a config-driven safe
  ceiling, not per-prefix tunables.

## Rebuild plan

Phased so each lands independently and is testable on `fe2`
(source of truth, `AGENTS.md`; one command at a time):

1. **Refactor, no behaviour change.** Split `ld-dn.c` into prefix
   discovery, a `policy` struct built from the current literals, and the
   emit path (env + stack + loader). Prove byte-for-byte identical env
   against the current binary. (Prerequisite for everything else.)
2. **Reader.** Add `openat`/`read`/`close` config reader + parser into
   the arena, behind `DN_CONFIG`; compiled defaults when unset. Add a
   `DN_DEBUG_CONFIG=1` dump of the resolved policy to fd 2.
3. **Ship the default file.** Add `native/ld-dn.conf` reproducing today's
   policy; copy it in `setup-runtime.sh`. Verify a prefix with and
   without the file behaves identically.
4. **Directives.** Implement `loader`, `lib-add`, `preload`, `env`,
   `unset`, `no-default-preload`, `shim-prefix`, then `[program]`
   blocks. Tests: append a lib, add an env var, strip one, per-program
   block, malformed line, oversized file.
5. **Wire the shim/tracer.** Add `DN_REDIRECT_PREFIXES` consumption to
   `path-redirect.c` and config-driven bind roots to the tracer.
6. **Benchmark.** Extend `scripts/bench/bench-tracer.sh` (or add a
   process-spawn microbench) to measure always-probe vs `DN_CONFIG`-gated
   on a spawn-heavy workload, and decide the cost contract from data.
7. **Docs.** Update `native/README.md`, `path-shim.md`'s env section,
   and this doc's status; run `scripts/tools/check-repo.py`.

Suggested order matches [`bind-only.md`](bind-only.md)'s style: land the
no-op refactor first, A/B the cost, only then let config drive the
shim.

## Status (0.5.2-prealpha)

Landed end to end. Evidence: `tests/ld-dn-config/run.sh` passes on-device
(compiled defaults, the shipped default file, explicit overrides,
per-program blocks, and a malformed config), and the live `~/.dn` prefix
shows a translated Debian `env` with the same four entries before and
after the rebuild. As built, against the plan above:

- **Implementation** — `native/ld-dn.c` splits into prefix discovery, a
  `policy` table (lib / preload / env / unset / shim-prefix), a
  freestanding reader (`openat` + `read` into a 16 KiB in-place buffer),
  and an emitter; the stack rebuild counts entries from the policy, not a
  literal `+5`. Defaults and `native/ld-dn.conf` reproduce the
  pre-0.5.2 environment exactly.
- **Config file** — `native/ld-dn.conf`, installed once by
  `setup-runtime.sh` to `$INSTDIR/etc/deb-native/ld-dn.conf` (never
  overwritten, so a prefix's edits survive a reinstall).
- **Directives** — `loader`, `lib-add`, `no-default-lib`, `preload`,
  `no-default-preload`, `env`, `unset`, `shim-prefix`, `[program]`.
- **Shim** — `native/path-redirect.c` reads `DN_REDIRECT_PREFIXES`; its
  compiled `/usr /etc /var /opt /root /lib /bin /sbin` set is the default
  when unset, and `shim-prefix` replaces it.
- **Diagnostics** — `DN_REDIRECT_DEBUG=1` dumps the resolved policy;
  config problems warn to stderr and the bad line is skipped, never
  fatal. `DN_NO_CONFIG` skips the read; `DN_CONFIG` relocates it.
- **Not done** — the `DN_CONFIG`-gated fast path (always-probe chosen for
  reachability), globbing in `[program]` names, and pointing the tracer
  at the same file (loader and shim only for now). `DN_EXTRA_LIB_PATH`
  and `DN_PRELOAD` remain the one-shot env overrides.

## Open questions

Resolved by the 0.5.2 implementation: always-probe (with `DN_NO_CONFIG`),
`DN_EXTRA_LIB_PATH` + `DN_PRELOAD` kept minimal, additive `lib-add`,
basename + path-suffix matching. Still open:

- **Cost contract** — always-probe is correct but unmeasured; run a
  spawn-heavy benchmark and only gate it if the `openat` shows.
- **Env override surface** — is `DN_EXTRA_LIB_PATH` + `DN_PRELOAD`
  enough, or does a general `DN_SET_<NAME>` / `DN_UNSET_<NAME>` earn its
  keep? Start minimal.
- **Ordering vs. `no-default-lib`** — is a caller-defined full lib list
  ever needed, or is additive always right?
- **Program matching** — basename + path-suffix sufficient, or does a
  real globbing need case appear? Keep globs out until one does.
- **Where `[program]` rules are authored** — hand-edited in the prefix,
  or generated by `custom/<package>.sh` / a translate-time hook so a
  package's needs travel with its install?
- **Can the tracer consume the same file directly** (it is a normal
  libc program), rather than only via ld-dn's env?
