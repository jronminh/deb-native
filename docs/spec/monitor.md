# The runtime's parts: law, catalog, monitor, executors, emu

<!-- template: templates/docs.template.md -->

Status: proposal (branch `dn-interp-surgical`). Supersedes the first "roles"
draft of this doc. Refines principle 3 of [`runtime.md`](runtime.md).

## Related docs

- [`runtime.md`](runtime.md) — the runtime spec this refines.
- [`tracer-rebuild.md`](tracer-rebuild.md) — rebuilding the tracer into
  `dn-monitor` (mechanism) + `dn-trace` (mapping executor).
- [`emulation.md`](emulation.md) — the operations the platform refuses, and
  "refuse, don't fake".
- [`../reference/syscall-boundary.md`](../reference/syscall-boundary.md) — the
  five cases a libc interposer cannot reach.

## The problem this settles

Two statements sat uneasily together: `dn-trace` is the ancestor of every
process (it installs the syscall filter), yet it was called a *peer* of
`dn-shim`. And "who catches every syscall?" had no answer that was not
`dn-trace` — which is not what `dn-trace` does. Finally, "who decides what an
operation should do?" was spread across the tracer, the shim and the catalog.

This separates **catching**, **deciding**, **routing** and **executing**, and
gives each a name.

## The parts

| part | is | its authority |
| --- | --- | --- |
| `dn-policy` | a static C library | **routing truth** (does the platform provide the operation?) and the **mapping law** (guest↔host, fake root, hardlink) + the shared state |
| the catalog ([`src/syscalls.tsv`](../../src/syscalls.tsv)) | data, **published from `dn-policy`** | per syscall: group, handling, gate-exempt — the routing table the monitor reads |
| `dn-monitor` | the monitor: catalog + a seccomp filter + the gate contract | **catches every syscall** and routes it |
| `dn-shim` | an in-process `LD_PRELOAD` executor | applies the mapping law to the calls a dynamic program makes through libc (fast) |
| `dn-trace` | a `ptrace` executor | applies the mapping law to the residual (what the shim cannot see) |
| `dn-emu` | the emulation suite | **the second authority**: for each refused/divergent operation it decides *emulate or refuse*, and owns its models |
| `dn-interp` | the program interpreter | bootstrap: delivers the shim and maps the gate page |
| the census | data, written at first boot | the per-device truth the routing and `dn-emu` consume |

## Two authorities, by domain

Deciding is not one thing:

- **`dn-policy`** decides what the *platform* does: provided, refused, or
  divergent. That is the routing truth, and it is also the mapping law.
- **`dn-emu`** decides what *it* can do: whether it can fake a refused or
  divergent operation. When it cannot, it **refuses** clearly (`EPERM` /
  `ENOSYS` + a reason), never a fiction — the boundary from
  [`emulation.md`](emulation.md), "Refuse, don't fake".

Everything else executes. `dn-shim` and `dn-trace` are **pure executors** of
the one mapping law; `dn-emu` is a decider because emulation is a *suite of
distinct subsystems*, not one law.

## Routing, in two taps

The monitor routes each syscall, but the filter and the dispatcher are not the
same decision:

1. **The filter (static, built from the catalog).** In the kernel, it cannot
   ask `dn-policy` at run time, so `dn-policy`'s classification is
   *materialised into the filter at build time*. It returns only:
   - from the gate page → **ALLOW** (the shim already did it);
   - in a handled group, not from the gate → **TRACE**;
   - otherwise → **ALLOW**.
2. **The dispatcher (runtime, per `dn-policy`).** When a call is TRACEd, the
   root asks `dn-policy` what the platform provides:
   - provided → **`dn-trace`** (map it);
   - refused / divergent → **`dn-emu`** (emulate it, or refuse).

So the monitor routes *by `dn-policy`*, twice: once compiled, once at run
time.

## The asymmetry: front-run vs dispatched

`dn-shim` and `dn-trace` are peers in **role** (both execute the mapping law),
but the monitor relates to them differently, and this must be kept:

- **`dn-shim` front-runs the monitor.** It acts *before* the syscall reaches
  the filter, in-process. The monitor never dispatches to it; it only
  *acknowledges* it (the gate mark → ALLOW).
- **`dn-trace` is dispatched by the monitor** (TRACE), as is `dn-emu`.

## The census (per-device truth)

Some of the platform's truth is static per (arch, API level) — the seccomp
allowlist, `CAP_SYS_ADMIN` gating `mount`/`unshare`. The rest varies by
device/version (user namespaces, `/dev/fuse`, raw ICMP, `open_by_handle_at`).
A one-time **first-boot census** measures the varying part:

- probe in a **forked child**, so a `SIGSYS`-killed probe kills only the child;
- record what each probe returned (`OK` / `EPERM` / `SIGSYS` / `EINVAL` /
  `ENOSYS`);
- store the result keyed by (arch, API level, build), so an Android update
  invalidates it;
- consume it in the routing and in `dn-emu`.

It is a bootstrap step, never on the hot path.

## Principle 3, restated

> **One `dn-policy`** (routing truth + the mapping law + the shared state),
> published as **the catalog**. **One `dn-monitor`** catches and routes every
> syscall. **Two executors** apply the mapping law: `dn-shim` (in-process,
> front-run) and `dn-trace` (`ptrace`, dispatched). **`dn-emu`** is a second
> authority: it emulates, or refuses, the operations the platform does not
> provide, from its own models.

## What this changes

- **Naming / docs**: `dn-trace` is no longer "the catcher"; `dn-monitor` (the
  catch/route) and `dn-emu` (the emulation suite) are named.
- **`dn-policy`**: keeps the mapping law and publishes the routing
  classification as the catalog. It does not absorb `dn-emu`'s models.
- **Code**: none is required to adopt the names. `dn-monitor` is the current
  filter subsystem; `dn-emu` is the current emulation code
  (`src/tracer/tracee/seccomp.c` and friends); the mapping executor is the
  rest of the tracer.
