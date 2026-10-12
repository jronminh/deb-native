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

## One source of truth: give the catalog shapes

The shim applies the law by *libc symbol*; the tracer by *syscall number*. Both
encode one spec. If [`src/syscalls.tsv`](../../src/syscalls.tsv) grows a
**shape** column (which arguments are paths, follow/nofollow, whether a stat
post applies, ...), then:

- the tracer is driven by the catalog instead of a hand-written switch;
- the shim's build can be checked against the same rows;
- `tools/check-syscalls.py` keeps the two in step.

This turns the trim into a *structure*, not just deleted files.

## Plan (later, discussed first)

1. Adopt the names in docs (`dn-monitor`, `dn-trace`); this file + `monitor.md`.
2. Make `dn-trace`'s path/identity handling call `dn-policy` (drop `path/`).
3. Drive `dn-trace` from the catalog (shapes) instead of the hard switch.
4. Move the emulable refused-operations into the shim for dynamic programs.
5. Cut the PRoot baggage.
6. Fold `dn-monitor`/`dn-trace` naming into the tree.
