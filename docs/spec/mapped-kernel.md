# The kernel, mapped into the prefix

The model the runtime exists to realise, and the rules that follow from it. How
the parts work is [`architecture.md`](../architecture.md); what it costs is
[`performance.md`](../performance.md).

## The model

The runtime runs an unmodified Debian tree directly on the Android kernel, with
no root, no namespaces and no `chroot`. It does not isolate the tree: it makes
the tree's paths **be** the program's view of the kernel. The Android kernel is
the engine and stays where it is; the runtime is a **mapping** -- a change of
coordinates -- that puts the tree's root where a program expects the kernel's
root (`/`) to be.

So the kernel is not emulated in the usual sense: nothing is *made up*. The
kernel does the work for real; what changes is the *frame of reference*. It is
called emulation only because the tree is not truly at the kernel's root -- that
is not real, so the mapping is.

The centre of the design is therefore the **mapping**, not the mechanism that
applies it. `dn-tiny` (the tracer) and `dn-lane.so` (the shim) are *engines*
that run the mapping; they can be replaced without changing it. The map is the
invariant.

## Why it can be complete

Everything a program observes of the operating system crosses the syscall
boundary: an action large or small ends in a call to the kernel. So the set of
coordinates a program can read -- paths, ids, fds, the mount view, the process
view -- lives in the syscall surface. That surface is finite (the arm64 set, a
few hundred calls) and enumerable, so the mapping can be *completed*: there is
an end to the work, unlike emulating a machine.

Three interfaces do not cross it, and none carries a coordinate, so none breaks
the completeness:

- the **vDSO** (user code the kernel maps) and the **vvar** page it reads;
- the cycle counter (`cntvct_el0`, `rdtsc`), a register;
- shared-memory rings (`io_uring`, `perf`), mostly refused on Android.

But completeness of the *surface* is not success. Three conditions decide it:

1. **Classify, do not special-case.** Each syscall belongs to a class (path,
   result, socket, exec, id, refused, none) answered by one rule; a few hundred
   separate answers contradict each other and a program finds the seam.
2. **Every reader.** A value is handled only when *every* place that reads it is
   mapped -- the path in and the path named out.
3. **The honest residue.** A mapping cannot carry a value the kernel fixes (the
   app's uid, a refused operation); a fake or a refusal stands in, and a fake
   must cover every reader or it is a lie. Map over fake, fake over nothing.

So covering every syscall is *sufficient for the surface* and makes success
possible; it is not automatic, and it holds only with **one law** -- which is
why Phase 0 comes first.

## The rules

1. **Map by default; fake is the residue; refuse when a fake would be a lie.**
   Use the real kernel wherever a mapping can carry the value. Fake only what
   cannot be mapped, and cover every reader that can see it. Where even a fake
   is a fiction a program can catch, refuse clearly (`EPERM`/`ENOSYS` + a
   reason). This is the old spec's "refuse, don't fake".
2. **Every reader.** A value is mapped only when *every* way a program reads it
   is mapped: a path in, and every place the kernel names a path out (`getcwd`,
   `readlink`, `/proc/*/{cwd,exe,fd/*,maps,mounts,mountinfo}`, `statx`, a
   directory listing, ...). One uncovered reader is a seam the program can find.
3. **No exceptions.** No program may touch the host's frame of reference. The
   loader, the exec gate, the `/proc` file names and fd targets all go through
   the same mapping; there is no "these are host paths, so skip the law" case.
4. **One mapping.** There is a single law (guest path <-> host path). The tracer
   and the shim must not each carry a copy: two copies can disagree, and the 0.7
   runtime died of exactly that.
5. **Binds are data.** Which guest paths are the prefix root, and which map to
   host directories, is the mapping's data -- one table, read by every engine,
   not rules re-derived in each place.
6. **Near-native, and as fast as possible.** The mapping costs, and a
   direct-kernel runtime accepts a cost below native -- but the mapping must be
   the cheapest it can be. The filter, the lane, the site patches and the loader
   hook are all this: fast paths to the one mapping. Optimise them, but only in
   service of the mapping.

## Reading the code through it

- An unimplemented reader is not a "missing feature"; it is a hole in the
  kernel that is supposed to sit in the prefix.
- A duplicated rule (the shim's resolver beside the tracer's) is debt, however
  well it performs.
- A special case that lets the host frame leak (the exec gate's host paths, a
  loader that assumes host paths) is a seam, not an optimisation.
- Prefer a mapping to a fake; prefer a clear refusal to a fiction.

## What this orders

The mapping must be *complete* and *single* before it is fast. So, in order:

1. **One law**, shared by the tracer, the shim and any early hook.
2. **The loader and the gate uniform** -- the gate hands the loader guest paths,
   so it is a program like any other, and the host/guest seam goes.
3. **The binds a table**, and the `/proc` files that name paths generated from
   it, so the view is self-consistent.
4. **Then the engines** -- the site patch, the cancellable `svc`, the `LD_AUDIT`
   loader hook -- as fast paths to the one mapping.

The speed work already done (the filter's tree, the shared cookie block, the
loader hook, the cancellable patch) is that fourth step: not wasted, just not
the root.

The roadmap that carries this out -- the two phases, mapping then disguising --
is [`plan.md`](plan.md).
