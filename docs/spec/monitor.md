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
- [`../reference/ptrace-interception.md`](../reference/ptrace-interception.md) —
  the ptrace and seccomp primitives, and how `dn-monitor` composes them.
- [`../reference/device-census.md`](../reference/device-census.md) — the census
  taken by hand on one device: which syscalls kill a process in the app domain.

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
| `dn-monitor` | the monitor — the tree's **root process**: the catalog + a seccomp filter + the gate contract + the ptrace core | **catches every syscall** and routes it; the single decision point |
| `dn-shim` | an in-process `LD_PRELOAD` executor | applies the mapping law to the calls a dynamic program makes through libc (fast) |
| `dn-trace` | a `ptrace` executor | applies the mapping law to the residual (what the shim cannot see) |
| `dn-emu` | the emulation suite | **the second authority**: for each refused/divergent operation it decides *emulate or refuse*, and owns its models |
| `dn-interp` | the program interpreter | bootstrap: delivers the shim and maps the gate page |
| the census | data, written at first boot | the per-device truth the routing and `dn-emu` consume |

## Two authorities, by domain

Deciding is not one thing:

- **`dn-policy`** decides what the *platform* does: provided, refused, or
  divergent. That is the routing truth, and it is also the mapping law. It
  holds **no** knowledge of what can be emulated — that is not a fact about the
  platform, and the law must not grow emu's models.
- **`dn-emu`** decides what *it* can do: whether it can fake a refused or
  divergent operation. When it cannot, it **refuses** clearly (`EPERM` /
  `ENOSYS` + a reason), never a fiction — the boundary from
  [`emulation.md`](emulation.md), "Refuse, don't fake".

Everything else executes. `dn-shim` and `dn-trace` are **pure executors** of
the one mapping law. `dn-emu` **owns yet another set of models** (emulation is a
*suite of distinct subsystems*, not one law), but it is no router either:
`dn-monitor` is the **single decision point** — it catches, routes and assigns;
`dn-trace` and `dn-emu` carry out exactly what they are given.

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

### The cascade, stated precisely

Coverage is a cascade — a later tier sees only what an earlier one cannot:

```
dynamic program ──(libc symbol)──► dn-shim   (front-run, in-process)
static / raw    ─┐
                 └──(svc #0)──────► dn-monitor (filter) ──► dn-trace │ dn-emu
```

- **`dn-shim`** — programs that use a dynamic loader and dynamic paths: the
  preemptible libc symbols. It **front-runs** the monitor (in-process, before
  the filter); the monitor never dispatches to it, it only *acknowledges* it
  through the gate mark.
- **`dn-trace`** — the residual the shim cannot see (libc-internal, an explicit
  `syscall()`, an inline `svc`, a static binary). Mostly *provided* calls whose
  arguments are **guest-shaped** (a path, a uid) that it maps. It also carries
  the mechanism (the exec gate, process tracking) and is the *path* by which a
  refused call reaches `dn-emu`.
- **`dn-emu`** — refused and **divergent** operations: what Android does not
  provide (`set*id` trapped, `rseq`, `mount`, SysV IPC, `io_uring`) and what it
  provides with different behaviour.

That order is **coverage**, not **routing**. The routing is a **partition by
`dn-policy`**, decided *before* either executor runs:

```
provided            -> dn-trace  (map it)
refused / divergent -> dn-emu    (emulate it, or refuse)
```

`dn-trace` is a **pure executor** of the one mapping law. `dn-emu` is a
**second authority**, not an executor: it decides, from its own models, whether
it can emulate the operation at all. So "`dn-emu` handles what `dn-trace` could
not" is the wrong picture twice over: a refused call was never `dn-trace`'s to
try, and if `dn-emu` cannot emulate it, the bottom of the cascade is a clear
**refusal** (`EPERM` / `ENOSYS` + a reason, [`emulation.md`](emulation.md)
"Refuse, don't fake") — never a fallback back to `dn-trace`.

Two consequences:

- `dn-emu` never acts alone: the refused set is *delivered* to it by the
  tracer's `SIGSYS` interception, so `dn-monitor` is both the filter (for the
  provided set) and the catcher of `SIGSYS` (for the refused set).
- a classification can be **per device** (`statx` provided on a new kernel and
  `ENOSYS` on an old one; `set_robust_list` trapped on one device and allowed on
  another) — which is what the census measures.

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

## Where the model leaks

Failing closed is by design. These are the places it does not, or where the
"one monitor" picture is thinner than it sounds.

**The filter is the enforcer, and it outlives the tracer.** A tracer that dies
does not kill its tracees — no `PTRACE_O_EXITKILL` is set today — so they are
detached, and they keep the seccomp filter, which is inherited and cannot be
removed. What follows is measured (WSL, kernel 6.18; a `SECCOMP_RET_TRACE`
filter and no tracer attached: `getpid` and `openat` return `-1/ENOSYS`, while
`write` succeeds):

| after the monitor dies | what happens |
| --- | --- |
| a refused/divergent call (`rseq`, `set*id`, …) | `SIGSYS` with no tracer to answer it — **the process dies** |
| a provided call in the catalog (`openat`, `stat`, …) | `SECCOMP_RET_TRACE` with no tracer — the kernel answers **`ENOSYS`** |
| a call outside the catalog | runs as always (it was always `ALLOW`ed) |

So a tree that loses its monitor is not "unmapped", it is **stuck**: it cannot
reach a path and it cannot `exec`. The one real gap is the last row — a
detached process can keep doing *non-path* work (compute, write to an
already-open fd). Setting `PTRACE_O_EXITKILL` would make "the monitor dies, the
tree dies" literally true. **Open decision.**

**Holes in the tracing itself:**

- `clone(..., CLONE_UNTRACED)` — the kernel will not force-trace that child, and
  `PTRACE_O_TRACECLONE` does not override it. The monitor must *refuse the flag*,
  not merely see the call.
- `io_uring` — submitted operations are executed by kernel workers, outside the
  tracee, so neither the filter nor the tracer sees their paths
  ([`dn-interp-shim.md`](dn-interp-shim.md): "io_uring bypasses every tier").
  Android blocks `io_uring_setup`, so it fails anyway.
- the gate page is trusted **by address**: any call issued from `P_GATE`
  (`0x100000000`, fixed and public) is `ALLOW`ed. That is the fast path's
  contract — but anything mapped there can mark its own calls.
- a path-taking syscall **missing from the catalog** is `ALLOW`ed and runs
  unmapped. The catalog and the census are what keep that set empty.

**Not holes, limits, by design (Gate B/C):** mount/namespaces/overlayfs/FUSE,
`/proc/net`, ports < 1024; and processes outside the tree (another Android
domain, the host's own tools) were never under the monitor. There is no
isolation promise: fake root is a *view*, the kernel still runs the app uid.

**A thin boundary.** The monitor and the processes it monitors share **one
Android uid**; SELinux (same domain), Yama (where enabled) and the filter's own
`ptrace` interception are all that stands between a tree program and its
monitor.

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
- **Code**: adopting the names needs no behaviour change. `dn-monitor` now
  lives in `src/tracer/monitor/` — `monitor.c`/`monitor.h`, the filter + gate
  rule, `dn_monitor_install()` / `dn_monitor_sysnum_flags()` (moved out of
  `syscall/seccomp.c`, which is gone); `dn-emu` has a home of its own
  (`src/dn-emu/`, the dispatcher's `dn_emu_syscall()` — a stub for now) while
  the emulation code it will *own* still runs inside the tracer
  (`src/tracer/tracee/seccomp.c` and friends); the mapping executor is the rest
  of the tracer. What is *not* adopted yet is tap 2: the dispatcher
  (`dn-trace` vs `dn-emu`) and the catalog as the filter's TRACE source
  instead of PRoot's hand-maintained `proot_sysnums`.
