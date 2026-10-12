# The loader-centric runtime: `ld-dn`, `dn-shim`, `dn-trace`

<!-- template: templates/docs.template.md -->

Status: design (dry branch `ld-dn-surgical`, unbuilt). On this branch the
in-process enforcement point is a **shim**, not a patched glibc: glibc stays
stock, `ld-dn` is the program's interpreter, and the shim owns the gate page.
`dn-policy` and `dn-trace` from `main` are reused unchanged. This supersedes
the `dn-policy-glibc-wiring` fast path of [`overlay.md`](overlay.md) for this
branch.

## Related docs

- [`native.md`](native.md) — the execution model this serves: near-full,
  near-native.
- [`overlay.md`](overlay.md) — the current runtime; `dn-policy` and `dn-trace`
  are reused unchanged.
- [`../reference/syscall-boundary.md`](../reference/syscall-boundary.md) — the
  five cases, and why the tracer (not the shim) reaches them.
- [`../reference/android-platform.md`](../reference/android-platform.md) — the
  three enforcement gates a syscall can fail at.

## Goal

A stock Debian `.deb` installs and its programs run unmodified, near-native,
with no per-package patching. The fast path stays in-process — a path call
costs a handful of instructions, a call that touches no path costs nothing —
but it is carried by a **shim** linked against `dn-policy`, not by glibc source
patched in-process. glibc is stock Debian glibc (plus the minimal Android
baseline it needs to start at all); there is no `dn-policy` wiring inside it.

## Components

| component | what it is | its job |
| --- | --- | --- |
| `ld-dn` | freestanding aarch64 static-PIE (revived from the pre-0.6 history) | the program's interpreter: derive `$DN`, read `ld-dn.conf`, rebuild the environment, map `P_GATE`, map the tree's `ld.so`, fix `AT_BASE`, jump. No re-exec. |
| `dn-shim.so` | `LD_PRELOAD` shared object (the old `path-redirect`, rewired) | the in-process router: `dn-policy` path/identity translation, its syscalls issued from `P_GATE` |
| `dn-policy` | static C library (unchanged) | the one place path-mapping, fake-root and hardlink rules live |
| `dn-trace` | the tracer (unchanged) | the root process: the shared seccomp filter, the exec gate, the emulation of refused operations, the catalog |
| tree glibc | stock Debian glibc (+ Android baseline) | normal execution; no in-process policy |

"One policy, two enforcement points" holds: `dn-policy` is linked into both
`dn-shim.so` (in-process) and `dn-trace` (ptrace), so the two can never
disagree.

## Boot

```
HOST runs the contract entry
  └─ TREE/usr/lib/deb-native/dn-trace
       │  derive_prefix(): /proc/self/exe → TREE
       │  LD-DN        = TREE/usr/lib/deb-native/ld-dn
       │  TREE-LD.SO   = TREE/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
       │  dn_policy_init(TREE, RT); bindings(TREE→/, /dev /proc /sys); cwd; exe
       └─ launch_process([" /usr/bin/bash", "…/init.sh"])
             │ fork
             └─ CON  ptrace(TRACEME); SIGSTOP
                     enable_syscall_filtering()   ← one seccomp BPF, inherited
                     execvp(/usr/bin/bash)
```

The filter is installed once at the root and inherited across every
`fork`/`exec`; there is no per-process adoption step.

## Exec gate

```
[FILTER] execve/execveat → TRACE
  └─ translate_execve_enter()
       ├─ expand_shebang()                       rule 1
       ├─ classify_exec() by ELF header:
       │     no PT_INTERP           → rule 2  STATIC
       │     PT_INTERP = ld-linux    → rule 3  GLIBC
       │     PT_INTERP other         → rule 4  FOREIGN
       ├─ remember /proc/self/exe = host_path
       └─ rule 3 → exec:  LD-DN --argv0 orig0 host_path args…
          rule 2/4 → exec:  host_path   (path translated, exec direct)
```

`PT_INTERP` is never repointed: the gate rewrites the exec arguments in the
tracee's memory, so a package's files on disk are byte-identical to the `.deb`.

## Three lanes

```
=== LANE 1 — dynamic (glibc)  [rule 3] ===
kernel:  LD-DN --argv0 orig0 host_path args…
            │
            │  ld-dn (freestanding, no libc)
            ├─ derive $DN from itself (/proc/self/exe)
            ├─ read $DN/etc/deb-native/ld-dn.conf   (policy-as-data, [program] blocks)
            ├─ build env from scratch:               ← survives an empty envp
            │      LD_PRELOAD     = $DN/usr/lib/deb-native/dn-shim.so
            │      LD_LIBRARY_PATH = tree libdirs (+ caller merge)
            │      DN_INSTDIR, COMPILER_PATH, …
            ├─ map  P_GATE (0x100000000, "svc #0; ret", r-x)   ← moved out of rtld
            ├─ map  TREE-LD.SO, fix AT_BASE, br x16
            └─ no re-exec → /proc/self/exe stays the program
            │
            ▼  ld.so = the tree's real glibc loader (UNPATCHED) loads the program
            │
        ┌───┴──────────────────────────────────────────────────┐
        │  dn-shim.so  (LD_PRELOAD)                             │
        │  each path/identity call:                             │
        │     dn_policy_translate_path()/… (in-process)         │
        │     → issue svc FROM P_GATE                           │
        └───────────────────────────────────────────────────────┘
            │
[FILTER]    ├─ path/identity from gate-IP  → ALLOW
            ├─ execve                      → TRACE
            └─ everything else             → ALLOW
   ⇒ near-native, in-process, no glibc patching

   what the shim cannot reach (NSS __open_nocancel, gconv, ld.so internals):
       not from the gate → TRACE → dn-trace → dn-policy


=== LANE 2 — static  |  LANE 3 — raw syscall (no loader)  [rule 2] ===
kernel:  host_path   (no interpreter rewrite)
  [FILTER] every path/identity not from the gate → TRACE
           execve → TRACE
  dn-trace: ptrace → dn_policy (translate_path, fake-root, link2symlink)
  lane 3 also: Gate A SIGSYS → handle_seccomp_event → emulation / ENOSYS
  ⚠ io_uring bypasses every tier
```

## Delegation: what the shim cannot reach

The shim only sees preemptible dynamic symbols. Everything else reaches the
filesystem without crossing one — mapped in
[`../reference/syscall-boundary.md`](../reference/syscall-boundary.md):

| # | case | shim? | reached by |
| --- | --- | --- | --- |
| 1 | ordinary libc path call | yes | shim |
| 2 | libc-internal / statically-bound (NSS, loader internals) | no | tracer |
| 3 | explicit `syscall()` | no (could) | tracer (or a shim interposer) |
| 4 | inline `svc #0` (Go, Rust, static-musl) | no | tracer |
| 5 | fully static executable | no | tracer |

Crucially, this needs **no per-binary routing and no install-time tagging**. The
filter's gate-IP rule does the sorting: a call issued from `P_GATE` was already
translated in-process and is `ALLOW`ed; a call from anywhere else is `TRACE`d.
NSS reads `/etc/passwd` internally, so they do not come from the gate and fall
to `dn-trace`, which translates them through `dn-policy` like any other path.
The old `dn-run` / `make-launchers` / `scan-direct-syscalls` machinery is not
needed; the shared filter replaces it.

## What changes vs `main`

| piece | change |
| --- | --- |
| exec gate rule 3 | target `ld-dn` instead of `RT/ld.so` (one variable / one call) |
| `ld-dn` | recover `native/ld-dn.c`/`ld-dn.conf` from the pre-0.6 history; add the build step |
| `dn-shim.so` | rewrite the old `path-redirect.c`: call `dn-policy`, issue syscalls from `P_GATE` |
| gate page | remove it from `elf/rtld.c`; `ld-dn` maps it instead |
| `dn-policy-glibc-wiring.patch` | dropped from the build pipeline |
| `dn-glibc-android.patch` | keep only the Android-baseline pieces stock glibc needs to start |
| `src/syscalls.tsv` | `glibc` column becomes the shim's wiring; `gate=yes` for rows the shim issues from the gate; libc-internal rows stay `no` |
| kept unchanged | `dn-policy`, `dn-trace` (filter, exec gate, emulation), the catalog, boot/packaging/CI |

## Non-goals

- A glibc source fork for policy (the whole point of this branch).
- Per-package patching or per-binary routing.
- Isolation, TUN/firewall, services, systemd — unchanged from
  [`overlay.md`](overlay.md)'s hard limits.
