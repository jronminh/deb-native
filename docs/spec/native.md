# Native Debian execution: the complete model

<!-- template: templates/docs.template.md -->

The target this project is built around: a Debian `arm64` userland that behaves
nearly like a real Debian system to the programs it runs, and runs them at
nearly the host device's own speed. This doc states that target, why it is
reachable at all, and the four pieces it rests on — a near-full overlay, a
near-native host, a complete front-end for the binary, and a back-end built to
keep the speed. It is a synthesis of [`overlay.md`](overlay.md),
[`emulation.md`](emulation.md) and [`prefix.md`](prefix.md), not a
replacement for them. Status: the target, with what is built and what is open.

## Contents

- [The goal](#the-goal)
- [Why native is possible](#why-native-is-possible)
- [The overlay, near-full](#the-overlay-near-full)
- [The front-end: the ABI a Debian binary sees](#the-front-end-the-abi-a-debian-binary-sees)
- [The back-end: near-native by tier](#the-back-end-near-native-by-tier)
- [Performance model and measurement](#performance-model-and-measurement)
- [Hard limits and refusals](#hard-limits-and-refusals)
- [Roadmap to the target](#roadmap-to-the-target)
- [Status](#status)

## Related docs

- [`overlay.md`](overlay.md) — the runtime (`dn-policy`, `dn-glibc`,
  `dn-trace`) this model is enforced by.
- [`emulation.md`](emulation.md) — the backend that answers operations the
  platform refuses.
- [`prefix.md`](prefix.md) — the tree and the tarball.
- [`../../TODO.md`](../../TODO.md) — the scheduled work (P4 is the measurement).

## The goal

Two properties, one design:

- **Near-full** — a `.deb` package installs and its programs run unmodified;
  every interface a program observes agrees on a single Debian view.
- **Near-native** — those programs run at nearly the host device's speed.

Completeness is bought at the **boundary** (the syscall/libc layer) and speed
is protected by keeping that boundary work on the **cheapest tier**. The two
constraints together decide the architecture: nothing may be added to the hot
path, and nothing a program can observe may be left without an answer.

## Why native is possible

- **Same instruction set.** A Debian `arm64` binary is executed directly by the
  device CPU. There is no instruction translation — the failing of `qemu-user`
  is absent by construction.
- **Real kernel.** Programs run on the Android/Linux kernel; syscalls are real
  kernel entries, not interpreted.
- **Real glibc.** The prefix ships glibc built from Debian's own source (the two
  patches in `patches/`), so libc is native machine code.
- **The only added work is interception**, and on the fast path it happens
  in-process: a path call costs a handful of instructions; a call that touches
  no path costs **nothing**. This is what separates this design from `proot`
  (a `ptrace` context switch on every syscall) and `qemu` (per-instruction
  translation).

Native speed for CPU-bound work is therefore not an optimization to chase but a
consequence of not adding a layer over the instructions. The engineering is
entirely about the *syscall* boundary.

## The overlay, near-full

The overlay is the runtime that presents a coherent Debian view without
modifying packages ([`overlay.md`](overlay.md)). "Near-full" means the boundary
covers **every interface a program observes**, not just paths:

- path translation — dn-policy longest-prefix mapping and absolute-symlink
  resolution;
- identity — fake root with the owner store (`user.dn.*` xattr), `get*id`
  answered from it;
- hardlinks — link2symlink with a refcount DB and the stat fixup;
- the operations Android refuses — emulated or substituted
  ([`emulation.md`](emulation.md));
- and **every reader** of that state, so no interface reports the unfaked truth
  ([`emulation.md`](emulation.md), "Consistency").

The measure of completeness is behavioural, not a package count: a program's
own probes of the environment (mounts, `/proc`, capabilities, `uname`) must
return what it expects.

## The front-end: the ABI a Debian binary sees

"Front-end" here is the contract through which a Debian binary meets the
system. For ordinary packages to work unmodified, the binary must be able to
rely on:

- **the exec path** — every `exec` goes through the exec gate, which runs a
  glibc-dynamic program via `RT/ld.so`; static and foreign loaders route to the
  tracer. `PT_INTERP` is never repointed after the build
  ([`prefix.md`](prefix.md)).
- **libc** — the prefix's patched glibc, whose path- and identity-taking
  functions consult dn-policy in-process.
- **the environment** — the entry sets `HOME`, `PATH`, `TMPDIR`, `DN_INSTDIR`
  and scrubs the host's (a Bionic `LD_PRELOAD` must not reach a glibc shell).
- **the filesystem view** — `/etc`, `/usr`, `/var` resolve into the tree;
  `/proc`, `/sys`, `/dev`, `/tmp` are the host's (declared exceptions).
- **the process view** — `uname`, `uid`, capabilities and mounts are normalized
  to the Debian expectation (the *divergent* class of
  [`emulation.md`](emulation.md)).

This is the front-end to the **binary**. It is distinct from the front-end to
**Android**: that one is the app's broker (dn-shell's `dn-android.md`, "The
bridge"), and the two meet where a prefix asks Android for something only
Android can give (DNS, notifications, storage).

## The back-end: near-native by tier

Performance is a property of **where each call is handled**
([`overlay.md`](overlay.md) principle 4):

| tier | what it covers | cost |
| --- | --- | --- |
| **dn-glibc** (fast path) | dynamic glibc binaries; path/identity rewritten in-process | a few instructions per path call; zero for non-path calls |
| **dn-trace** (fallback) | static, Go, raw `svc #0`, foreign loaders, Gate-A `SIGSYS` | `ptrace`: two context switches per trapped syscall |

Rules that keep the design near-native:

1. Keep the common case on the fast path — never route a dynamic binary to
   `ptrace` for ordinary work.
2. **Cache** path and symlink resolution so a lookup is not a filesystem hit
   per call.
3. Keep the **seccomp filter narrow**; exempt the syscalls the gate page
   already translates.
4. Keep new emulation (port translation, `/proc` synthesis, capabilities) **off
   the hot path**.
5. Do no needless metadata work — owner faking only where a program observes it.
6. Remove the overlay's own dependencies from the path (e.g. static-link
   `libtalloc` into `dn-trace`).

## Performance model and measurement

Three regimes, with different expectations:

- **CPU-bound** (compress, compile): no layer over the instructions, so
  **≈ native**.
- **path/syscall-bound**: dn-glibc's cost is per path call and small; dn-trace
  is where it degrades, so the goal is to keep this class on the fast path.
- **I/O**: the same page cache and block layer as the host — no second VFS.

Decision metric: **the fraction of syscalls served by each tier**, and the
per-trapped-syscall cost. Measurement plan: microbenchmarks per regime,
compared native (a Linux phone on the same hardware) vs dn-glibc vs dn-trace,
driven by the per-tier counters of [`../../TODO.md`](../../TODO.md) P4.

## Hard limits and refusals

Some things can be neither near-full nor native
([`../reference/android-platform.md`](../reference/android-platform.md)):
user namespaces off kernel-wide, `mount`/`/dev/fuse`, SELinux on `/proc/sys`
and netlink, raw sockets in the root netns, ports below 1024, setuid/setgid and
file capabilities, TUN. Each is classified in
[`emulation.md`](emulation.md), "Two kinds of hard limit": forbidden-outright
(emulate or refuse), restricted/divergent (normalize), forbidden-with-fallback
(route to the older syscall). Security-relevant state is **refused, never
faked**.

## Roadmap to the target

The work to reach the target, in the order that protects the goal. Measure
before optimizing: the only evidence for "near-native" is a per-tier
measurement.

**0. Measure first** ([`../../TODO.md`](../../TODO.md) P4). Per-tier,
per-syscall counters in `dn-trace`, plus microbenchmarks for the three regimes
(CPU-bound, path/syscall-bound, I/O) against native on the same hardware.
Output: a per-tier call-distribution table for install, compile and run.

**1. Complete the fast path** ([`overlay.md`](overlay.md) P2) — the decisive
step for near-native, because anything not on it falls to `ptrace`:

- route every path/identity syscall through the gate page; flip the remaining
  `src/syscalls.tsv` rows to `gate=yes` (including `bind` and the identity
  group);
- override the `syscalls.list`-generated wrappers (`mkdirat`/`unlinkat`/
  `symlinkat`/`readlinkat`/`fchownat`/`chdir`/`chroot`/`*xattr`) so they take
  the gate;
- handle `syscall_cancel.S`'s cancellation markers;
- **cache** path and symlink resolution (today every lookup hits the
  filesystem directly);
- narrow the seccomp filter to the syscalls that still need the tracer.

Done when: a real workload never routes ordinary work to `ptrace`.

**2. Make the front-end near-full.**

- finish exec-gate rule 4 (a foreign loader);
- **normalize** the restricted/divergent class in both tiers (`uname`, `/proc`
  values, `rlimit`/cgroup, W^X `mprotect`) — the silent one
  ([`emulation.md`](emulation.md), "Two kinds of hard limit");
- settle the prefix-ABI contract (entry, env, the `dn-host` socket and its
  version, lifecycle, hooks).

**3. Emulate what the platform refuses** ([`emulation.md`](emulation.md)). A
virtual mount table; `/proc/sys`, `/proc/net`, `/proc/self/*` synthesis;
privileged-port translation; raw ICMP; capability synthesis; (stretch)
userspace overlay copy-up with merged reads. Rule: every one of these stays
**off the hot path**.

**4. Clear the hot path** ([`overlay.md`](overlay.md) P3, plus hygiene).
Static-link `libtalloc` into `dn-trace` so it has no runtime dependency; remove
the old shim and the now-duplicated `dn_fake_root()` / `fakeroot_sysnums` once
dn-policy carries them ("one policy, two enforcement points").

**Order:** 0 → 1 → 2/3 (parallel) → 4. The invariant throughout: nothing may be
added to the hot path.

## Status

This doc is the target and its conditions, not a claim already met.
[`overlay.md`](overlay.md) is the current runtime; the phased work to close the
gap is in [Roadmap to the target](#roadmap-to-the-target) above.
