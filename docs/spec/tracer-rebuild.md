# Rebuilding the tracer: `dn-monitor` + `dn-trace`

<!-- template: templates/docs.template.md -->

Status: proposal (branch `dn-interp-surgical`). A survey and a plan; no code
changed here.

## Related docs

- [`monitor.md`](monitor.md) — the roles (`dn-monitor` catches, `dn-shim` /
  `dn-trace` enforce).
- [`runtime.md`](runtime.md) — the runtime spec this restructures.
- [`emulation.md`](emulation.md) — the backend that fakes what the platform
  refuses.
- [`../reference/syscall-boundary.md`](../reference/syscall-boundary.md) — the
  five cases an interposer cannot reach.
- [`../../src/tracer/`](../../src/tracer) — the fork this is about.

## Why now

`src/tracer/` is a reduced fork of `termux/proot` ("fork-lite", imported in
`6ecf78a`, pruned in `6bd83d8`/`d793aaa`, loader removed in `ad99433`). It is
first-class project code, but it still **owns policy logic** it should not:
it translates paths, fakes root, does hardlinks and emulates refused
operations with its *own* code, in parallel with what `dn-policy` and the shim
do.

The shim changes the calculus. `dn-shim.so` covers the path/identity surface
for dynamic programs. That lets the tracer stop owning policy and shrink to
**mechanism + a thin, data-driven applier for the residual** (static, raw
`svc`, libc-internal, exec) — a cut much deeper than "port fewer ptrace
files".

## The three parts

| part | is | owns |
| --- | --- | --- |
| `dn-policy` | the law + shared state | the guest↔host transformation, fake root, hardlink — and, later, the emulated systems' models |
| `dn-monitor` | the mechanism | catch every syscall (filter) + the ptrace core (event loop, tracee tree, memory, registers) |
| `dn-trace` | the policy applier (ptrace side) | read the args from the tracee, call `dn-policy`, write the result back |
| `dn-shim` | the policy applier (in-process side) | the same law, front-running the monitor |

## Inventory (`src/tracer/`, ~14.4k lines, talloc excluded)

**Mechanism → `dn-monitor`**

| file | lines |
| --- | --- |
| `ptrace/{ptrace,wait,user}.c` | 1229 |
| `tracee/event.c` | 946 |
| `tracee/tracee.c` | 928 |
| `tracee/mem.c` | 691 |
| `tracee/reg.c` | 399 |
| `syscall/seccomp.c` (filter + gate rule) | 720 |
| `syscall/{syscall,sysnum,chain,pipe_shadow,rlimit}.c`, `sysnums-arm64.h` | ~1400 |
| `cli/`, `arch.h`, `compat.h`, `attribute.h` | ~800 |

**Policy applier → `dn-trace`**

| file | lines |
| --- | --- |
| `syscall/enter.c` | **2920** |
| `syscall/exit.c` | 837 |
| `execve/{enter,exit,shebang,elf,aoxp}.c` | 1238 |
| `path/{path,binding,canon,proc,temp,glue,f2fs-bug}.c` | **2978** |
| `syscall/{dn-identity,dn-owner}.c` | 425 |
| `tracee/seccomp.c` (emulation) | 680 |
| `tracee/statx.c`, `syscall/socket.c` | 356 |

**Cut candidates**: `extension/` (framework, hooks are no-ops), `path/{canon,
temp,f2fs-bug}`, `syscall/{pipe_shadow,rlimit}` — PRoot scope, not ours.

## What the shim lets us cut

| the tracer used to | now |
| --- | --- |
| translate paths on the traced path (bindings) | call `dn_policy_translate_path()`: read the path, translate, write it back |
| fake root (identity + owner + stat post) | `dn-policy` (shared with the shim) |
| hardlink (link2symlink) | `dn-policy` (shared) |
| emulate refused operations (mount/unshare/…) | the **shim** for dynamic programs (those are public libc symbols); the tracer only for static/raw |
| a 2920-line per-syscall switch | a **catalog-driven** applier: per-syscall shape → `dn-policy` |

The tracer's logic was a *second copy* of what `dn-policy` and the shim do.
With the shim, that copy goes away.

## Target sizes

```
dn-monitor  ≈ ptrace core + filter + event/tracee/mem/reg   ~ 4-6k  (mechanism)
dn-trace    ≈ exec gate + read→dn-policy→write (data-driven)  ~ 1-2k  (from ~9.5k)
dn-policy   ≈ the law + state, shared with the shim          (exists)
cut         ≈ extension/, canon/temp/f2fs, pipe_shadow, rlimit, most of enter.c
```

## The irreducible core (`dn-monitor`)

What cannot be cut, because nothing else can do it:

- read/write strings in the tracee and **allocate scratch memory** in it;
- **rewrite `execve` / argv** (`execve/aoxp.c`) — seccomp user-notification can
  inspect and inject fds but cannot modify arguments;
- the **event loop** and fork/clone/exec tracking;
- install the filter.

That is the ptrace core, and it is why the fork reuses PRoot rather than a
clean-room tracer ([`../../docs/`](../../docs), the fork-lite decision).

## Cost: the unpatched loader vs a patched `rtld` (measured)

The in-process fast path (the shim) never reaches the tracer. What does reach
it, per program start, is: `exec`, and every **path/identity syscall the shim
cannot see** — including `ld.so`'s own, because an unpatched `ld.so` is not a
dn-glibc and its calls do not come from the gate page. So "no `rtld` patch"
has a price, in `ld.so`'s startup.

A probe on x86_64 (WSL) counted those syscalls per start on a stock glibc, and
timed the same starts:

| program | path/identity syscalls / start | start, untraced | trace every syscall |
| --- | --- | --- | --- |
| `/bin/true` | 4 | ~0.75 ms | ~5.3 ms |
| `bash -c :` | 15 | ~2.5 ms | ~16 ms |
| `python3 -c pass` | 89 | ~14.3 ms | ~62 ms |

Reading:

- the traced set is **small for ordinary programs** (a handful), larger only
  for import-heavy tools;
- tracing *every* syscall is a large multiplier, but the filter TRACEs only the
  path/identity group plus `exec`, so the real cost tracks the first column, not
  the third;
- with the shim covering the program's own libc calls, the residual is `ld.so`'s
  startup calls — a handful. An extra exec (the launcher hop) measured
  **~0.2–0.5 ms** on the same probe.

Conclusion: **not patching `rtld` is acceptable** — the cost is a fraction of a
start, not milliseconds. A **minimal `rtld` patch** (map `P_GATE`, issue from
it) removes the residual and needs **no install hook**: it is a build-time glibc
patch, shipped inside the tarball, and much smaller than `main`'s wiring (two
files vs forty-three). Defer it until an on-device measurement says the residual
is worth it. Likewise, the second exec of the launcher (option 2) is cheap
enough not to decide the design on speed alone.

The probe is x86_64/WSL: indicative, not the device.

## One source of truth: give the catalog shapes

The shim applies the law by *libc symbol*; the tracer by *syscall number*. Both
encode one spec. If [`src/syscalls.tsv`](../../src/syscalls.tsv) grows a
**shape** column (which arguments are paths, follow/nofollow, whether a stat
post applies, ...), then:

- the tracer is driven by the catalog instead of a hand-written switch;
- the shim's build can be checked against the same rows;
- `tools/check-syscalls.py` keeps the two in step.

This turns the trim into a *structure*, not just deleted files.

## Code plan

Two tracks. A finishes the shim runtime; B rebuilds the tracer. Each step is
its own commit, build-verified, and leaves the tree usable.

### Track A — finish the shim runtime

- **A1 (P2b-3)** — make `dn-interp` a **launcher**: it sets the environment
  (`LD_PRELOAD=dn-shim.so`, `LD_LIBRARY_PATH`, `DN_PROG`) and `execve`s the
  tree's `ld.so` (`--argv0 …`), so the kernel does the load. `P_GATE` stays in
  the **shim** (an exec wipes any mapping, so the launcher cannot map it, and
  the loader stays unpatched). Drop `dn-policy-glibc-wiring.patch` from the
  pipeline (`scripts/glibc/`, `.github/workflows/build-glibc.yml`); recompute
  `src/syscalls.tsv`'s `glibc`/`gate` columns.
- **A2 (P3)** — trim `dn-glibc-android.patch` to the Android baseline;
  confirm a stock `libc6` boots.

### Track B — rebuild the tracer

- **B1 — the `dn-emu` hook (stub).** Introduce `dn-emu` as its own component
  with one entry point the dispatcher calls for a refused/divergent syscall;
  for now it answers "not handled", so the existing emulation keeps doing the
  work. No behaviour change — it marks where the subsystems land. (`dn-emu`
  is a dummy until a subsystem is designed.)
- **B2 — `dn-trace` as a mapping executor.** Replace the tracer's own path and
  identity handling with `dn-policy`: read the path from the tracee, call
  `dn_policy_translate_path()`, write it back; take fake root and hardlink
  from `dn-policy` too. Delete the tracer's `path/` translation and its
  private fakeroot/hardlink.
- **B3 — catalog shapes.** Extend [`src/syscalls.tsv`](../../src/syscalls.tsv)
  with a shape column (which arguments are paths, follow/nofollow, whether a
  stat post applies) and drive `dn-trace` from it instead of the hand-written
  switch.
- **B4 — split `dn-monitor`.** Separate the mechanism (filter + ptrace core)
  from the policy limb; the dispatcher routes by `dn-policy`.
- **B5 — the census.** A first-boot probe (forked child), keyed by (arch, API
  level, build); consumed by the routing and by `dn-emu`.
- **B6 — cut the PRoot baggage.** `extension/`, `syscall/pipe_shadow.c`,
  `syscall/rlimit.c`, `path/{canon,temp,f2fs-bug}`.
