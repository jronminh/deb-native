# The runtime's roles: `dn-policy`, `dn-monitor`, `dn-shim`, `dn-trace`

<!-- template: templates/docs.template.md -->

Status: proposal (branch `dn-interp-surgical`). Refines principle 3 of
[`runtime.md`](runtime.md); runtime.md is updated to match once this is
agreed.

## Related docs

- [`runtime.md`](runtime.md) — the runtime spec this refines.
- [`dn-interp-shim.md`](dn-interp-shim.md) — the loader-centric branch.
- [`../reference/syscall-boundary.md`](../reference/syscall-boundary.md) — the
  five cases a libc interposer cannot reach.

## The problem this settles

Two statements sat uneasily together:

- `dn-trace` is the ancestor of every process: it installs the syscall filter,
  and every process in the tree descends from it.
- `dn-trace` and `dn-shim` are "two enforcement points" of one policy
  ([`runtime.md`](runtime.md), principle 3) — i.e. peers.

If `dn-trace` is the root and `dn-shim` is a per-process accelerator, they are
not peers, and treating them as such is what made the picture murky. And the
question "who catches every syscall?" had no answer that was not `dn-trace` —
which is not what `dn-trace` does.

This note separates **catching/routing** from **enforcing**, and names the
catcher.

## The roles

| component | is | does |
| --- | --- | --- |
| `dn-policy` | a static C library | the transformation rules (guest↔host, fake root, hardlink) **and** the cross-process state (owner store, link counts) |
| the catalog ([`src/syscalls.tsv`](../../src/syscalls.tsv)) | data | the routing table: which syscalls are in the handled groups, which are gate-exempt |
| `dn-monitor` | the runtime's syscall monitor: the catalog + a seccomp filter + the gate-page contract | **catches every syscall** and routes it |
| `dn-interp` | the program interpreter | delivers the shim (environment) and maps the gate page |
| `dn-shim` | an in-process `LD_PRELOAD` handler | applies dn-policy to the calls a dynamic program makes through libc, then issues them from the gate |
| `dn-trace` | the fallback handler (ptrace) | applies dn-policy to the calls the shim cannot see; also the tree's root (an implementation necessity, not a role) |

## The catcher is `dn-monitor`, not `dn-trace`

The thing that observes **every** syscall is the seccomp filter: the kernel
evaluates it inline for each syscall, at no per-call process cost. It is
**generated from the catalog**, **installed by the tree's root**, and
**defined by the gate-page contract** (`P_GATE`, a fixed address shared by
three builders: this filter, `dn-interp` which maps it, and `dn-shim` which
issues from it).

That whole is a component of the runtime — a `dn-*`, like `dn-policy` (a
library), `dn-shim` (a shared object) and `dn-interp` (a binary) — only its
mechanism lives in the kernel instead of user space. Calling `dn-trace` "the
catcher" was the error: `dn-trace` does not catch anything; it handles what the
filter routes to it.

## Routing (dn-monitor's rules)

- from the gate page → **ALLOW** (the shim already did it in-process);
- in a handled group, not from the gate → **TRACE** → `dn-trace`;
- otherwise → **ALLOW**.

The classification is the **catalog**; the transformation is **dn-policy** —
two halves of one spec.

## The asymmetry: front-run vs dispatched

`dn-shim` and `dn-trace` are peers in **role** (both apply dn-policy), but the
monitor relates to them differently, and this must be kept:

- **`dn-shim` front-runs the monitor.** It acts *before* the syscall reaches
  the filter, in-process. The monitor never dispatches to it; it only
  *acknowledges* it (the gate mark → ALLOW). dn-monitor cannot "hand a call to
  the shim" — the shim has already taken it.
- **`dn-trace` is dispatched by the monitor** (TRACE). This is the only path by
  which the monitor hands work out.

## Layering

```
        dn-policy  (rules + shared state)      catalog  (routing table)
                    \                        /
                     \                      /
                      dn-monitor  (catch every syscall, route)
                      /                        \
             dn-shim  (front-run,                dn-trace  (dispatched,
              in-process, acked via the gate)     ptrace, the residual)
                              ^
                     dn-interp  (delivers dn-shim, maps the gate)
```

"One dn-policy, two enforcement points" becomes, precisely:

> **One `dn-policy` (rules + shared state). One `dn-monitor` catches every
> syscall and routes it by the catalog. Two handlers apply dn-policy:
> `dn-shim` in-process (front-run, acknowledged via the gate) and `dn-trace`
> (dispatched for what the shim cannot see).**

## Implementation level

For now `dn-monitor` is a **subsystem of `dn-trace`**: `dn-trace` is the tree's
root, and it builds and installs the filter from the catalog. A distinct
monitor *process* is possible — `PTRACE_SEIZE` from a sibling, or
`SECCOMP_RET_USER_NOTIF` with a listener — but it is not chosen: the roles are
clear without paying for a second root.

## What this changes

- **Naming / docs**: `dn-trace` is no longer called "the catcher"; `dn-monitor`
  is named. Principle 3 of [`runtime.md`](runtime.md) is reworded to match.
- **Code**: none is required to adopt the names — `dn-monitor` *is* the current
  filter subsystem. A later step may split files or naming to make it explicit.
- **Contract**: `P_GATE` and the catalog stay the two things the filter, the
  interpreter and the shim must all agree on.
